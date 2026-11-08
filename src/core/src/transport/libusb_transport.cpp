#include "bifrost/transport/libusb_transport.hpp"

#include "bifrost/transport/iokit_registry.hpp"

#include <libusb.h>

#include <limits>
#include <string>
#include <utility>

namespace bifrost::transport {
namespace {

ErrorCode classify(int code)
{
    switch (code) {
    case LIBUSB_ERROR_NO_DEVICE:
    case LIBUSB_ERROR_NOT_FOUND:
        return ErrorCode::usbDisconnected;
    case LIBUSB_ERROR_TIMEOUT:
        return ErrorCode::usbTimeout;
    case LIBUSB_ERROR_PIPE:
        return ErrorCode::usbStall;
    case LIBUSB_ERROR_ACCESS:
    case LIBUSB_ERROR_BUSY:
        return ErrorCode::usbAccess;
    default:
        return ErrorCode::usbOther;
    }
}

Error libusbError(const std::string& action, int code)
{
    return makeError(classify(code),
                     action + ": " + libusb_error_name(code), code);
}

TransferType translateTransferType(std::uint8_t attributes)
{
    switch (attributes & LIBUSB_TRANSFER_TYPE_MASK) {
    case LIBUSB_TRANSFER_TYPE_ISOCHRONOUS: return TransferType::isochronous;
    case LIBUSB_TRANSFER_TYPE_BULK: return TransferType::bulk;
    case LIBUSB_TRANSFER_TYPE_INTERRUPT: return TransferType::interrupt;
    default: return TransferType::control;
    }
}

std::vector<AlternateSetting> readAlternates(
    const libusb_config_descriptor& configuration)
{
    std::vector<AlternateSetting> alternates;
    for (std::uint8_t i = 0; i < configuration.bNumInterfaces; ++i) {
        const auto& usbInterface = configuration.interface[i];
        for (int j = 0; j < usbInterface.num_altsetting; ++j) {
            const auto& descriptor = usbInterface.altsetting[j];
            AlternateSetting alternate{
                descriptor.bInterfaceNumber, descriptor.bAlternateSetting,
                descriptor.bInterfaceClass, descriptor.bInterfaceSubClass,
                descriptor.bInterfaceProtocol, {}};
            for (std::uint8_t k = 0; k < descriptor.bNumEndpoints; ++k) {
                const auto& endpoint = descriptor.endpoint[k];
                alternate.endpoints.push_back(
                    {endpoint.bEndpointAddress,
                     translateTransferType(endpoint.bmAttributes),
                     endpoint.wMaxPacketSize});
            }
            alternates.push_back(std::move(alternate));
        }
    }
    return alternates;
}

struct DeviceListDeleter {
    void operator()(libusb_device** devices) const noexcept
    {
        libusb_free_device_list(devices, 1);
    }
};
using DeviceList = std::unique_ptr<libusb_device*, DeviceListDeleter>;

struct ConfigDeleter {
    void operator()(libusb_config_descriptor* descriptor) const noexcept
    {
        libusb_free_config_descriptor(descriptor);
    }
};
using ConfigPtr = std::unique_ptr<libusb_config_descriptor, ConfigDeleter>;

class LibusbClaimedInterface final : public ClaimedInterface {
public:
    LibusbClaimedInterface(std::shared_ptr<libusb_context> context,
                           libusb_device_handle* handle,
                           OdinInterface selection, bool kernelDriverDetached)
        : context_(std::move(context)), handle_(handle), selection_(selection),
          kernelDriverDetached_(kernelDriverDetached)
    {
    }

    ~LibusbClaimedInterface() override
    {
        (void)release();
        libusb_close(handle_);
    }

    const OdinInterface& selection() const noexcept override
    {
        return selection_;
    }

    Expected<std::size_t> bulkWrite(const std::uint8_t* data, std::size_t size,
                                    unsigned int timeoutMs) override
    {
        if (!claimed_) {
            return makeError(ErrorCode::illegalState,
                             "USB interface was already released");
        }
        if ((data == nullptr && size != 0) ||
            size > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
            return makeError(ErrorCode::invalidArgument,
                             "USB bulk write buffer is invalid");
        }
        int transferred = 0;
        const int result = libusb_bulk_transfer(
            handle_, selection_.endpointOut, const_cast<unsigned char*>(data),
            static_cast<int>(size), &transferred, timeoutMs);
        if (result != LIBUSB_SUCCESS) {
            return libusbError("USB bulk write failed", result);
        }
        return static_cast<std::size_t>(transferred);
    }

    Expected<std::size_t> bulkRead(std::uint8_t* data, std::size_t size,
                                   unsigned int timeoutMs) override
    {
        if (!claimed_) {
            return makeError(ErrorCode::illegalState,
                             "USB interface was already released");
        }
        if (data == nullptr || size == 0 ||
            size > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
            return makeError(ErrorCode::invalidArgument,
                             "USB bulk read buffer is invalid");
        }
        int transferred = 0;
        const int result =
            libusb_bulk_transfer(handle_, selection_.endpointIn, data,
                                 static_cast<int>(size), &transferred,
                                 timeoutMs);
        if (result != LIBUSB_SUCCESS) {
            return libusbError("USB bulk read failed", result);
        }
        return static_cast<std::size_t>(transferred);
    }

    Status sendZeroLength(unsigned int timeoutMs) override
    {
        if (!claimed_) {
            return makeError(ErrorCode::illegalState,
                             "USB interface was already released");
        }
        int transferred = 0;
        const int result = libusb_bulk_transfer(
            handle_, selection_.endpointOut, nullptr, 0, &transferred,
            timeoutMs);
        if (result != LIBUSB_SUCCESS) {
            return libusbError("Zero-length USB transfer failed", result);
        }
        return success();
    }

    Status drainZeroLength(unsigned int timeoutMs) override
    {
        if (!claimed_) {
            return makeError(ErrorCode::illegalState,
                             "USB interface was already released");
        }
        // A 1-byte read, as Heimdall does (BridgeManager.cpp:655-658).
        unsigned char byte = 0;
        int transferred = 0;
        const int result = libusb_bulk_transfer(
            handle_, selection_.endpointIn, &byte, 1, &transferred, timeoutMs);
        if (transferred == 0 &&
            (result == LIBUSB_SUCCESS || result == LIBUSB_ERROR_TIMEOUT)) {
            return success();
        }
        if (result != LIBUSB_SUCCESS) {
            return libusbError("Optional zero-length read failed", result);
        }
        return makeError(ErrorCode::protocolViolation,
                         "Unexpected data where only an optional empty packet "
                         "was allowed");
    }

    Status release() override
    {
        if (!claimed_) {
            return success();
        }
        claimed_ = false;
        const int result =
            libusb_release_interface(handle_, selection_.interfaceNumber);
        // Hand the device back to the macOS CDC driver only after the release
        // has completed: on darwin, re-attaching re-enumerates the device and
        // libusb re-claims every interface still marked claimed
        // (darwin_restore_state), which is what auto-detach does on release
        // and why it leaked the interface (hardware test, 2026-11-08).
        if (kernelDriverDetached_) {
            kernelDriverDetached_ = false;
            (void)libusb_attach_kernel_driver(handle_, selection_.interfaceNumber);
        }
        if (result != LIBUSB_SUCCESS && result != LIBUSB_ERROR_NO_DEVICE) {
            return libusbError("Failed to release USB interface", result);
        }
        return success();
    }

private:
    std::shared_ptr<libusb_context> context_;
    libusb_device_handle* handle_{};
    OdinInterface selection_;
    bool claimed_{true};
    bool kernelDriverDetached_{};
};

// Reads the device's configuration without libusb_get_active_config_descriptor.
// On darwin that call reaches get_configuration_index, which dereferences the
// cached IOKit device without a NULL check; libusb sets it to NULL when a
// device re-enumerates (darwin_devices_detached, v1.0.29), and bifrost-usbd
// segfaulted there six times on 2026-11-08. libusb_get_config_descriptor
// checks for NULL and returns LIBUSB_ERROR_OTHER instead. Odin devices have a
// single configuration, so index 0 is the active one; anything else is refused.
Expected<ConfigPtr> readConfiguration(libusb_device* device,
                                      const libusb_device_descriptor& descriptor)
{
    if (descriptor.bNumConfigurations != 1) {
        return makeError(ErrorCode::usbOther,
                         "The Samsung device reports " +
                             std::to_string(descriptor.bNumConfigurations) +
                             " USB configurations; only single-configuration devices are "
                             "supported");
    }
    libusb_config_descriptor* config = nullptr;
    const int result = libusb_get_config_descriptor(device, 0, &config);
    if (result != LIBUSB_SUCCESS || config == nullptr) {
        return libusbError("Could not read the Samsung device's USB configuration",
                           result != LIBUSB_SUCCESS ? result : LIBUSB_ERROR_OTHER);
    }
    return ConfigPtr(config);
}

bool libusbAtLeast(int major, int minor, int micro)
{
    const libusb_version* version = libusb_get_version();
    if (version->major != major) {
        return version->major > major;
    }
    if (version->minor != minor) {
        return version->minor > minor;
    }
    return version->micro >= micro;
}

int LIBUSB_CALL hotplugEntry(libusb_context*, libusb_device*,
                             libusb_hotplug_event, void* userData)
{
    static_cast<LibusbTransport*>(userData)->notifyHotplug();
    return 0; // keep the callback registered
}

} // namespace

LibusbTransport::LibusbTransport(libusb_context* context)
    : context_(context, [](libusb_context* value) { libusb_exit(value); })
{
}

LibusbTransport::~LibusbTransport()
{
    stopHotplug();
}

Expected<std::unique_ptr<LibusbTransport>> LibusbTransport::create()
{
    libusb_context* context = nullptr;
    const int result = libusb_init(&context);
    if (result != LIBUSB_SUCCESS) {
        return libusbError("Failed to initialize libusb", result);
    }
    // Darwin kernel-driver detach exists since libusb 1.0.25 and the darwin
    // zero-length-packet fix since 1.0.28 (libusb ChangeLog).
    if (!libusbAtLeast(1, 0, 28)) {
        libusb_exit(context);
        return makeError(ErrorCode::usbOther,
                         "libusb 1.0.28 or newer is required");
    }
    return std::unique_ptr<LibusbTransport>(new LibusbTransport(context));
}

Expected<std::shared_ptr<libusb_context>> LibusbTransport::freshContext()
{
    libusb_context* context = nullptr;
    const int result = libusb_init(&context);
    if (result != LIBUSB_SUCCESS) {
        return libusbError("Failed to initialize libusb", result);
    }
    return std::shared_ptr<libusb_context>(context,
                                           [](libusb_context* value) { libusb_exit(value); });
}

Expected<UsbEnumeration> LibusbTransport::enumerate()
{
    auto context = freshContext();
    if (!context) {
        return std::move(context).error();
    }
    libusb_device** raw = nullptr;
    const ssize_t count = libusb_get_device_list(context->get(), &raw);
    if (count < 0) {
        return libusbError("Failed to enumerate USB devices",
                           static_cast<int>(count));
    }
    DeviceList devices(raw);

    UsbEnumeration enumeration;
#if defined(__APPLE__)
    if (auto registry = countSamsungDevicesInIokitRegistry(); !registry) {
        enumeration.warnings.push_back(registry.error().message);
    }
#endif
    for (ssize_t i = 0; i < count; ++i) {
        libusb_device_descriptor descriptor{};
        if (libusb_get_device_descriptor(raw[i], &descriptor) !=
                LIBUSB_SUCCESS ||
            descriptor.idVendor != kSamsungVendorId) {
            continue;
        }
        UsbDeviceInfo info;
        info.identity = {descriptor.idVendor, descriptor.idProduct,
                         libusb_get_bus_number(raw[i]),
                         libusb_get_device_address(raw[i])};
        // Only Download Mode devices need their configuration (and only they
        // are claimed). In Android mode the SM-N920I reports 2 configurations.
        if (isDownloadModeProductId(descriptor.idProduct)) {
            if (auto config = readConfiguration(raw[i], descriptor); config) {
                info.alternates = readAlternates(**config);
                info.odinInterface = findOdinInterface(info.alternates);
            } else {
                enumeration.warnings.push_back(config.error().message);
            }
        }
        enumeration.devices.push_back(std::move(info));
    }
    return enumeration;
}

Expected<std::unique_ptr<ClaimedInterface>> LibusbTransport::claim(
    const UsbIdentity& identity)
{
    if (identity.vendorId != kSamsungVendorId) {
        return makeError(ErrorCode::invalidArgument,
                         "Refusing to claim a non-Samsung USB device");
    }
    if (!isDownloadModeProductId(identity.productId)) {
        return makeError(ErrorCode::invalidArgument,
                         "Refusing to claim a Samsung device that is not in Download Mode");
    }
    auto context = freshContext();
    if (!context) {
        return std::move(context).error();
    }
    libusb_device** raw = nullptr;
    const ssize_t count = libusb_get_device_list(context->get(), &raw);
    if (count < 0) {
        return libusbError("Failed to enumerate USB devices",
                           static_cast<int>(count));
    }
    DeviceList devices(raw);

    for (ssize_t i = 0; i < count; ++i) {
        libusb_device_descriptor descriptor{};
        if (libusb_get_device_descriptor(raw[i], &descriptor) !=
                LIBUSB_SUCCESS ||
            descriptor.idVendor != identity.vendorId ||
            descriptor.idProduct != identity.productId ||
            libusb_get_bus_number(raw[i]) != identity.busNumber ||
            libusb_get_device_address(raw[i]) != identity.deviceAddress) {
            continue;
        }

        auto config = readConfiguration(raw[i], descriptor);
        if (!config) {
            return std::move(config).error();
        }
        const auto selection = findOdinInterface(readAlternates(**config));
        if (!selection) {
            return makeError(ErrorCode::usbOther,
                             "No CDC Data interface with one bulk IN and one "
                             "bulk OUT endpoint was found");
        }

        libusb_device_handle* handle = nullptr;
        int result = libusb_open(raw[i], &handle);
        if (result != LIBUSB_SUCCESS) {
            return libusbError("Failed to open the Samsung USB device", result);
        }
        // macOS binds AppleUSBACMData to the Odin interface (hardware test
        // T0, 2026-11-08). Detach it explicitly instead of using libusb's
        // auto-detach, which on darwin (a) claims the interface anyway if
        // the detach fails, sharing the bulk pipes with the kernel driver
        // (darwin_capture_claim_interface), and (b) leaks the interface on
        // release. Detaching needs root or the com.apple.vm.device-access
        // entitlement (T2a: LIBUSB_ERROR_ACCESS without them).
        bool detached = false;
        const int active = libusb_kernel_driver_active(handle, selection->interfaceNumber);
        if (active == 1) {
            result = libusb_detach_kernel_driver(handle, selection->interfaceNumber);
            if (result != LIBUSB_SUCCESS) {
                libusb_close(handle);
                return libusbError(
                    "Could not detach the macOS CDC driver from the Samsung interface "
                    "(this needs the Bifrost Studio USB helper or root); refusing to share "
                    "the interface with the kernel driver",
                    result);
            }
            detached = true;
        } else if (active < 0 && active != LIBUSB_ERROR_NOT_SUPPORTED) {
            libusb_close(handle);
            return libusbError("Could not check for a kernel driver on the Samsung interface",
                               active);
        }
        result = libusb_claim_interface(handle, selection->interfaceNumber);
        if (result != LIBUSB_SUCCESS) {
            if (detached) {
                (void)libusb_attach_kernel_driver(handle, selection->interfaceNumber);
            }
            libusb_close(handle);
            return libusbError(
                "Could not claim the Samsung CDC Data interface (close other "
                "USB tools; on macOS this needs the Bifrost Studio helper or root)",
                result);
        }
        result = libusb_set_interface_alt_setting(
            handle, selection->interfaceNumber, selection->alternateSetting);
        if (result != LIBUSB_SUCCESS) {
            libusb_release_interface(handle, selection->interfaceNumber);
            if (detached) {
                (void)libusb_attach_kernel_driver(handle, selection->interfaceNumber);
            }
            libusb_close(handle);
            return libusbError("Could not select the Odin alternate setting",
                               result);
        }
        return std::unique_ptr<ClaimedInterface>(
            new LibusbClaimedInterface(*context, handle, *selection, detached));
    }
    return makeError(ErrorCode::usbDisconnected,
                     "The selected Samsung device is no longer connected");
}

void LibusbTransport::notifyHotplug()
{
    HotplugCallback callback;
    {
        std::lock_guard<std::mutex> lock(hotplugMutex_);
        callback = hotplugCallback_;
    }
    if (callback) {
        callback();
    }
}

bool LibusbTransport::startHotplug(HotplugCallback callback)
{
    if (!libusb_has_capability(LIBUSB_CAP_HAS_HOTPLUG)) {
        return false;
    }
    stopHotplug();
    {
        std::lock_guard<std::mutex> lock(hotplugMutex_);
        hotplugCallback_ = std::move(callback);
    }
    const int result = libusb_hotplug_register_callback(
        context_.get(),
        static_cast<libusb_hotplug_event>(
            LIBUSB_HOTPLUG_EVENT_DEVICE_ARRIVED |
            LIBUSB_HOTPLUG_EVENT_DEVICE_LEFT),
        LIBUSB_HOTPLUG_NO_FLAGS, kSamsungVendorId, LIBUSB_HOTPLUG_MATCH_ANY,
        LIBUSB_HOTPLUG_MATCH_ANY, hotplugEntry, this, &hotplugHandle_);
    if (result != LIBUSB_SUCCESS) {
        return false;
    }
    hotplugRegistered_ = true;
    hotplugStop_ = false;
    hotplugThread_ = std::thread([this] {
        while (!hotplugStop_) {
            timeval timeout{0, 250000};
            libusb_handle_events_timeout_completed(context_.get(), &timeout,
                                                   nullptr);
        }
    });
    return true;
}

void LibusbTransport::stopHotplug()
{
    if (!hotplugRegistered_) {
        return;
    }
    hotplugStop_ = true;
    libusb_hotplug_deregister_callback(context_.get(), hotplugHandle_);
    if (hotplugThread_.joinable()) {
        hotplugThread_.join();
    }
    hotplugRegistered_ = false;
    std::lock_guard<std::mutex> lock(hotplugMutex_);
    hotplugCallback_ = nullptr;
}

} // namespace bifrost::transport
