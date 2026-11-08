#include "bifrost/service/usb_service.hpp"

#include "bifrost/service/wire.hpp"

#include <algorithm>
#include <mutex>
#include <string>
#include <vector>

namespace bifrost::service {

struct UsbService::Impl : std::enable_shared_from_this<UsbService::Impl> {
    struct Peer {
        Xpc connection;
        std::unique_ptr<transport::ClaimedInterface> claimed;
    };

    std::shared_ptr<transport::UsbTransport> transport;
    // Enumerate, claim and release never overlap: claim/release re-enumerate
    // the phone (kernel-driver detach/attach), and libusb's darwin backend
    // crashed reading a device mid re-enumeration (NULL priv->device in
    // get_configuration_index; SM-N920I session, 2026-11-08).
    std::mutex topology;
    std::mutex mutex;
    std::vector<std::shared_ptr<Peer>> peers;
    std::vector<std::shared_ptr<Peer>> watchers;
    bool hotplugStarted{};
    bool shutDown{};

    void handle(const std::shared_ptr<Peer>& peer, xpc_object_t message);
    void drop(const std::shared_ptr<Peer>& peer);
    void broadcastDevicesChanged();
    void send(xpc_object_t request, const Xpc& reply);
};

namespace {

xpc_connection_t asConnection(const Xpc& x)
{
    return static_cast<xpc_connection_t>(x.get());
}

Error notClaimed()
{
    return makeError(ErrorCode::illegalState, "No interface is claimed on this connection");
}

std::optional<unsigned int> timeoutOf(xpc_object_t message)
{
    const auto timeout = getUint(message, "timeout");
    if (!timeout || *timeout == 0 || *timeout > kMaximumTimeoutMs) {
        return std::nullopt;
    }
    return static_cast<unsigned int>(*timeout);
}

Error badRequest(const std::string& what)
{
    return makeError(ErrorCode::invalidArgument, "USB helper: invalid " + what);
}

} // namespace

void UsbService::Impl::send(xpc_object_t request, const Xpc& reply)
{
    (void)request;
    if (reply) {
        xpc_connection_send_message(xpc_dictionary_get_remote_connection(request), reply.get());
    }
}

void UsbService::Impl::handle(const std::shared_ptr<Peer>& peer, xpc_object_t message)
{
    if (xpc_get_type(message) != XPC_TYPE_DICTIONARY) {
        return;
    }
    const auto op = getString(message, "op");
    const auto fail = [&](const Error& error) { send(message, replyError(message, error)); };

    if (op == "hello") {
        auto reply = replyOk(message);
        if (reply) {
            xpc_dictionary_set_uint64(reply.get(), "version", kWireVersion);
            xpc_dictionary_set_string(reply.get(), "role", "usbd");
        }
        return send(message, reply);
    }
    if (getUint(message, "version").value_or(0) != kWireVersion) {
        return fail(makeError(ErrorCode::protocolViolation, "USB helper: wire version mismatch"));
    }
    if (op == "enumerate") {
        std::unique_lock<std::mutex> topologyLock(topology);
        auto enumeration = transport->enumerate();
        topologyLock.unlock();
        if (!enumeration) {
            return fail(enumeration.error());
        }
        auto reply = replyOk(message);
        if (reply) {
            xpc_dictionary_set_value(reply.get(), "enumeration",
                                     encodeEnumeration(*enumeration).get());
        }
        return send(message, reply);
    }
    if (op == "claim") {
        if (peer->claimed) {
            return fail(makeError(ErrorCode::illegalState,
                                  "This connection already holds a claimed interface"));
        }
        auto identity = getIdentity(message);
        if (!identity) {
            return fail(badRequest("device identity"));
        }
        if (identity->vendorId != transport::kSamsungVendorId ||
            !transport::isDownloadModeProductId(identity->productId)) {
            return fail(makeError(ErrorCode::invalidArgument,
                                  "USB helper only claims Samsung devices in Download Mode"));
        }
        std::unique_lock<std::mutex> topologyLock(topology);
        auto claimed = transport->claim(*identity);
        topologyLock.unlock();
        if (!claimed) {
            return fail(claimed.error());
        }
        peer->claimed = std::move(*claimed);
        auto reply = replyOk(message);
        if (reply) {
            putSelection(reply.get(), peer->claimed->selection());
        }
        return send(message, reply);
    }
    if (op == "watch") {
        bool supported = true;
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (std::find(watchers.begin(), watchers.end(), peer) == watchers.end()) {
                watchers.push_back(peer);
            }
            if (!hotplugStarted) {
                std::weak_ptr<Impl> weak = shared_from_this();
                hotplugStarted = transport->startHotplug([weak] {
                    if (auto self = weak.lock()) {
                        self->broadcastDevicesChanged();
                    }
                });
                supported = hotplugStarted;
            }
        }
        auto reply = replyOk(message);
        if (reply) {
            xpc_dictionary_set_bool(reply.get(), "supported", supported);
        }
        return send(message, reply);
    }

    // Everything below needs a claimed interface.
    if (!peer->claimed) {
        return fail(notClaimed());
    }
    auto& interface = *peer->claimed;
    if (op == "release") {
        std::lock_guard<std::mutex> topologyLock(topology);
        auto released = interface.release();
        peer->claimed.reset();
        return send(message, released ? replyOk(message) : replyError(message, released.error()));
    }
    const auto timeout = timeoutOf(message);
    if (!timeout) {
        return fail(badRequest("timeout"));
    }
    if (op == "write") {
        xpc_object_t data = xpc_dictionary_get_value(message, "data");
        if (data == nullptr || xpc_get_type(data) != XPC_TYPE_DATA) {
            return fail(badRequest("write data"));
        }
        const auto size = xpc_data_get_length(data);
        if (size == 0 || size > kMaximumTransfer) {
            return fail(badRequest("write size"));
        }
        const auto* bytes = static_cast<const std::uint8_t*>(xpc_data_get_bytes_ptr(data));
        auto written = interface.bulkWrite(bytes, size, *timeout);
        if (!written) {
            return fail(written.error());
        }
        auto reply = replyOk(message);
        if (reply) {
            xpc_dictionary_set_uint64(reply.get(), "count", *written);
        }
        return send(message, reply);
    }
    if (op == "read") {
        const auto size = getUint(message, "size");
        if (!size || *size == 0 || *size > kMaximumTransfer) {
            return fail(badRequest("read size"));
        }
        std::vector<std::uint8_t> buffer(static_cast<std::size_t>(*size));
        auto received = interface.bulkRead(buffer.data(), buffer.size(), *timeout);
        if (!received) {
            return fail(received.error());
        }
        auto reply = replyOk(message);
        if (reply) {
            putData(reply.get(), "data", buffer.data(), std::min(*received, buffer.size()));
        }
        return send(message, reply);
    }
    if (op == "zlp" || op == "drain") {
        auto status = op == "zlp" ? interface.sendZeroLength(*timeout)
                                  : interface.drainZeroLength(*timeout);
        return send(message, status ? replyOk(message) : replyError(message, status.error()));
    }
    fail(badRequest("operation '" + op + "'"));
}

void UsbService::Impl::drop(const std::shared_ptr<Peer>& peer)
{
    if (peer->claimed) {
        std::lock_guard<std::mutex> topologyLock(topology);
        (void)peer->claimed->release();
        peer->claimed.reset();
    }
    std::lock_guard<std::mutex> lock(mutex);
    peers.erase(std::remove(peers.begin(), peers.end(), peer), peers.end());
    watchers.erase(std::remove(watchers.begin(), watchers.end(), peer), watchers.end());
}

void UsbService::Impl::broadcastDevicesChanged()
{
    std::vector<Xpc> targets;
    {
        std::lock_guard<std::mutex> lock(mutex);
        for (const auto& watcher : watchers) {
            targets.push_back(watcher->connection);
        }
    }
    auto event = Xpc::adopt(xpc_dictionary_create(nullptr, nullptr, 0));
    xpc_dictionary_set_string(event.get(), "event", "devices-changed");
    for (const auto& target : targets) {
        xpc_connection_send_message(asConnection(target), event.get());
    }
}

UsbService::UsbService(std::shared_ptr<transport::UsbTransport> transport)
    : impl_(std::make_shared<Impl>())
{
    impl_->transport = std::move(transport);
}

UsbService::~UsbService()
{
    std::vector<std::shared_ptr<Impl::Peer>> peers;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        impl_->shutDown = true;
        peers = impl_->peers;
    }
    for (const auto& peer : peers) {
        xpc_connection_cancel(asConnection(peer->connection));
    }
    impl_->transport->stopHotplug();
}

void UsbService::accept(xpc_connection_t connection)
{
    auto peer = std::make_shared<Impl::Peer>();
    peer->connection = Xpc::retain(connection);
    std::shared_ptr<Impl> impl = impl_;
    {
        std::lock_guard<std::mutex> lock(impl->mutex);
        if (impl->shutDown) {
            xpc_connection_cancel(connection);
            return;
        }
        impl->peers.push_back(peer);
    }
    xpc_connection_set_event_handler(connection, ^(xpc_object_t event) {
        if (xpc_get_type(event) == XPC_TYPE_ERROR) {
            impl->drop(peer);
            return;
        }
        impl->handle(peer, event);
    });
    xpc_connection_resume(connection);
}

} // namespace bifrost::service
