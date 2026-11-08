#ifndef BIFROST_TRANSPORT_TRANSPORT_HPP
#define BIFROST_TRANSPORT_TRANSPORT_HPP

#include "bifrost/common/error.hpp"
#include "bifrost/transport/descriptors.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace bifrost::transport {

inline constexpr std::uint16_t kSamsungVendorId = 0x04E8;

struct UsbIdentity {
    std::uint16_t vendorId{};
    std::uint16_t productId{};
    std::uint8_t busNumber{};
    std::uint8_t deviceAddress{};

    bool operator==(const UsbIdentity& other) const noexcept
    {
        return vendorId == other.vendorId && productId == other.productId &&
               busNumber == other.busNumber &&
               deviceAddress == other.deviceAddress;
    }
};

struct UsbDeviceInfo {
    UsbIdentity identity;
    std::vector<AlternateSetting> alternates;
    std::optional<OdinInterface> odinInterface;
};

struct UsbEnumeration {
    std::vector<UsbDeviceInfo> devices;
    std::vector<std::string> warnings;
};

// A claimed Odin CDC Data interface. Implementations must be safe to destroy
// on any path; destruction releases the interface.
class ClaimedInterface {
public:
    virtual ~ClaimedInterface() = default;
    virtual const OdinInterface& selection() const noexcept = 0;
    virtual Expected<std::size_t> bulkWrite(const std::uint8_t* data,
                                            std::size_t size,
                                            unsigned int timeoutMs) = 0;
    virtual Expected<std::size_t> bulkRead(std::uint8_t* data, std::size_t size,
                                           unsigned int timeoutMs) = 0;
    // Best-effort zero-length OUT transfer.
    virtual Status sendZeroLength(unsigned int timeoutMs)
    {
        (void)timeoutMs;
        return success();
    }
    // Best-effort read of an optional zero-length IN packet. A timeout or a
    // zero-length read is success; unexpected data is an error.
    virtual Status drainZeroLength(unsigned int timeoutMs)
    {
        (void)timeoutMs;
        return success();
    }
    virtual Status release() = 0;
};

using HotplugCallback = std::function<void()>;

class UsbTransport {
public:
    virtual ~UsbTransport() = default;
    virtual Expected<UsbEnumeration> enumerate() = 0;
    virtual Expected<std::unique_ptr<ClaimedInterface>> claim(
        const UsbIdentity& identity) = 0;
    // Invokes the callback (on an internal thread) whenever a Samsung device
    // arrives or leaves. Returns false when hot-plug is unsupported.
    virtual bool startHotplug(HotplugCallback callback)
    {
        (void)callback;
        return false;
    }
    virtual void stopHotplug() {}
};

} // namespace bifrost::transport

#endif
