#ifndef BIFROST_TRANSPORT_LIBUSB_TRANSPORT_HPP
#define BIFROST_TRANSPORT_LIBUSB_TRANSPORT_HPP

#include "bifrost/transport/transport.hpp"

#include <atomic>
#include <memory>
#include <mutex>
#include <thread>

struct libusb_context;

namespace bifrost::transport {

class LibusbTransport final : public UsbTransport {
public:
    static Expected<std::unique_ptr<LibusbTransport>> create();
    ~LibusbTransport() override;

    Expected<UsbEnumeration> enumerate() override;
    Expected<std::unique_ptr<ClaimedInterface>> claim(
        const UsbIdentity& identity) override;
    bool startHotplug(HotplugCallback callback) override;
    void stopHotplug() override;

    // Invoked from libusb's hot-plug callback; not part of the public API.
    void notifyHotplug();

private:
    explicit LibusbTransport(libusb_context* context);

    // A new libusb context for one enumeration or claim. Each one rescans
    // IOKit, so it never lists a device that has since re-enumerated or left
    // (the long-lived hot-plug context kept stale addresses and devices whose
    // IOKit object libusb had already released; 2026-11-08).
    static Expected<std::shared_ptr<libusb_context>> freshContext();

    std::shared_ptr<libusb_context> context_; // hot-plug only
    std::mutex hotplugMutex_;
    HotplugCallback hotplugCallback_;
    int hotplugHandle_{};
    bool hotplugRegistered_{};
    std::atomic<bool> hotplugStop_{false};
    std::thread hotplugThread_;
};

} // namespace bifrost::transport

#endif
