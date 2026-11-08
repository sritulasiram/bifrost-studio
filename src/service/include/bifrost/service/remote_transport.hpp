// UsbTransport implemented by the privileged USB helper over XPC. The agent
// uses it so the core never needs root. Each claimed interface gets its own
// helper connection; if the helper dies, the claim is gone and I/O fails with
// usbDisconnected (the device may be partially written, exactly as for a
// pulled cable).
#ifndef BIFROST_SERVICE_REMOTE_TRANSPORT_HPP
#define BIFROST_SERVICE_REMOTE_TRANSPORT_HPP

#include "bifrost/service/wire.hpp"
#include "bifrost/transport/transport.hpp"

#include <memory>
#include <mutex>
#include <string>

namespace bifrost::service {

class RemoteTransport final : public transport::UsbTransport {
public:
    // The helper's Mach service in the system domain (a LaunchDaemon).
    // `requirement` is the code-signing requirement the helper must meet.
    static std::unique_ptr<RemoteTransport> machService(std::string name,
                                                        std::string requirement);
    // An in-process endpoint (tests).
    static std::unique_ptr<RemoteTransport> endpoint(Xpc endpoint, std::string requirement = {});
    ~RemoteTransport() override;

    Expected<transport::UsbEnumeration> enumerate() override;
    Expected<std::unique_ptr<transport::ClaimedInterface>> claim(
        const transport::UsbIdentity& identity) override;
    bool startHotplug(transport::HotplugCallback callback) override;
    void stopHotplug() override;

    // Opens a new helper connection and checks the wire version.
    Expected<Xpc> connect(xpc_handler_t eventHandler = nullptr);

private:
    RemoteTransport() = default;

    std::string machName_;
    Xpc endpoint_;
    std::string requirement_;
    std::mutex mutex_;
    Xpc control_; // reused for enumerate
    Xpc watch_;   // hot-plug notifications
    // Shared with the watch connection's block; cleared by stopHotplug() so
    // no callback starts after it returns.
    struct HotplugState {
        std::mutex mutex;
        bool active{true};
        transport::HotplugCallback callback;
    };
    std::shared_ptr<HotplugState> hotplug_;
};

} // namespace bifrost::service

#endif
