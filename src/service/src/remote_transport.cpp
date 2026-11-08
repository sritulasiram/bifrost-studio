#include "bifrost/service/remote_transport.hpp"

#include <algorithm>
#include <cstring>

namespace bifrost::service {
namespace {

constexpr const char* kPeer = "USB helper";

xpc_connection_t asConnection(const Xpc& x)
{
    return static_cast<xpc_connection_t>(x.get());
}

// Control calls (connect, enumerate, claim): an unreachable helper is a
// service problem. I/O calls on a claimed interface: losing the helper loses
// the claim, which is a disconnect for recovery purposes.
Expected<Xpc> control(const Xpc& connection, const Xpc& request)
{
    return call(asConnection(connection), request.get(), kPeer, ErrorCode::serviceUnavailable,
                ErrorCode::serviceUnavailable);
}

Expected<Xpc> io(const Xpc& connection, const Xpc& request)
{
    return call(asConnection(connection), request.get(), kPeer, ErrorCode::usbDisconnected,
                ErrorCode::serviceUnavailable);
}

class RemoteInterface final : public transport::ClaimedInterface {
public:
    RemoteInterface(Xpc connection, transport::OdinInterface selection)
        : connection_(std::move(connection)), selection_(selection)
    {
    }
    ~RemoteInterface() override { (void)release(); }

    const transport::OdinInterface& selection() const noexcept override { return selection_; }

    Expected<std::size_t> bulkWrite(const std::uint8_t* data, std::size_t size,
                                    unsigned int timeoutMs) override
    {
        if (released_) {
            return makeError(ErrorCode::illegalState, "Interface already released");
        }
        const auto chunk = std::min(size, kMaximumTransfer);
        auto request = makeRequest("write");
        putData(request.get(), "data", data, chunk);
        xpc_dictionary_set_uint64(request.get(), "timeout", clamp(timeoutMs));
        auto reply = io(connection_, request);
        if (!reply) {
            return std::move(reply).error();
        }
        const auto count = getUint(reply->get(), "count");
        if (!count || *count > chunk) {
            return makeError(ErrorCode::protocolViolation, "USB helper reported a bad write count");
        }
        return static_cast<std::size_t>(*count);
    }

    Expected<std::size_t> bulkRead(std::uint8_t* data, std::size_t size,
                                   unsigned int timeoutMs) override
    {
        if (released_) {
            return makeError(ErrorCode::illegalState, "Interface already released");
        }
        auto request = makeRequest("read");
        xpc_dictionary_set_uint64(request.get(), "size", std::min(size, kMaximumTransfer));
        xpc_dictionary_set_uint64(request.get(), "timeout", clamp(timeoutMs));
        auto reply = io(connection_, request);
        if (!reply) {
            return std::move(reply).error();
        }
        const auto bytes = getData(reply->get(), "data");
        if (bytes.size() > size) {
            return makeError(ErrorCode::protocolViolation, "USB helper returned too many bytes");
        }
        if (!bytes.empty()) {
            std::memcpy(data, bytes.data(), bytes.size());
        }
        return bytes.size();
    }

    Status sendZeroLength(unsigned int timeoutMs) override { return simple("zlp", timeoutMs); }
    Status drainZeroLength(unsigned int timeoutMs) override { return simple("drain", timeoutMs); }

    Status release() override
    {
        if (released_) {
            return success();
        }
        released_ = true;
        auto reply = io(connection_, makeRequest("release"));
        xpc_connection_cancel(asConnection(connection_));
        if (!reply) {
            return std::move(reply).error();
        }
        return success();
    }

private:
    static std::uint64_t clamp(unsigned int timeoutMs)
    {
        return std::min<unsigned int>(std::max(timeoutMs, 1u), kMaximumTimeoutMs);
    }

    Status simple(const char* op, unsigned int timeoutMs)
    {
        if (released_) {
            return makeError(ErrorCode::illegalState, "Interface already released");
        }
        auto request = makeRequest(op);
        xpc_dictionary_set_uint64(request.get(), "timeout", clamp(timeoutMs));
        auto reply = io(connection_, request);
        if (!reply) {
            return std::move(reply).error();
        }
        return success();
    }

    Xpc connection_;
    transport::OdinInterface selection_;
    bool released_{};
};

} // namespace

std::unique_ptr<RemoteTransport> RemoteTransport::machService(std::string name,
                                                              std::string requirement)
{
    std::unique_ptr<RemoteTransport> transport(new RemoteTransport());
    transport->machName_ = std::move(name);
    transport->requirement_ = std::move(requirement);
    return transport;
}

std::unique_ptr<RemoteTransport> RemoteTransport::endpoint(Xpc endpoint, std::string requirement)
{
    std::unique_ptr<RemoteTransport> transport(new RemoteTransport());
    transport->endpoint_ = std::move(endpoint);
    transport->requirement_ = std::move(requirement);
    return transport;
}

RemoteTransport::~RemoteTransport()
{
    stopHotplug();
    std::lock_guard<std::mutex> lock(mutex_);
    if (control_) {
        xpc_connection_cancel(asConnection(control_));
    }
}

Expected<Xpc> RemoteTransport::connect(xpc_handler_t eventHandler)
{
    Xpc connection =
        endpoint_ ? Xpc::adopt(xpc_connection_create_from_endpoint(
                        static_cast<xpc_endpoint_t>(endpoint_.get())))
                  : Xpc::adopt(xpc_connection_create_mach_service(
                        machName_.c_str(), nullptr, XPC_CONNECTION_MACH_SERVICE_PRIVILEGED));
    if (!connection) {
        return makeError(ErrorCode::serviceUnavailable, "Could not create a USB helper connection");
    }
    if (!requirement_.empty() &&
        xpc_connection_set_peer_code_signing_requirement(asConnection(connection),
                                                         requirement_.c_str()) != 0) {
        return makeError(ErrorCode::invalidArgument, "Invalid USB helper requirement");
    }
    if (eventHandler != nullptr) {
        xpc_connection_set_event_handler(asConnection(connection), eventHandler);
    } else {
        xpc_connection_set_event_handler(asConnection(connection), ^(xpc_object_t) {});
    }
    xpc_connection_resume(asConnection(connection));
    auto hello = control(connection, makeRequest("hello"));
    if (!hello) {
        xpc_connection_cancel(asConnection(connection));
        return std::move(hello).error();
    }
    if (getUint(hello->get(), "version").value_or(0) != kWireVersion ||
        getString(hello->get(), "role") != "usbd") {
        xpc_connection_cancel(asConnection(connection));
        return makeError(ErrorCode::serviceUnavailable,
                         "The USB helper is a different version; reinstall Bifrost Studio");
    }
    return connection;
}

Expected<transport::UsbEnumeration> RemoteTransport::enumerate()
{
    Xpc connection;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!control_) {
            auto connected = connect();
            if (!connected) {
                return std::move(connected).error();
            }
            control_ = std::move(*connected);
        }
        connection = control_;
    }
    auto reply = control(connection, makeRequest("enumerate"));
    if (!reply) {
        if (reply.error().code == ErrorCode::serviceUnavailable) {
            std::lock_guard<std::mutex> lock(mutex_);
            if (control_.get() == connection.get()) {
                xpc_connection_cancel(asConnection(control_));
                control_ = Xpc();
            }
        }
        return std::move(reply).error();
    }
    xpc_object_t encoded = xpc_dictionary_get_value(reply->get(), "enumeration");
    if (encoded == nullptr || xpc_get_type(encoded) != XPC_TYPE_DICTIONARY) {
        return makeError(ErrorCode::protocolViolation, "USB helper sent no device list");
    }
    return decodeEnumeration(encoded);
}

Expected<std::unique_ptr<transport::ClaimedInterface>> RemoteTransport::claim(
    const transport::UsbIdentity& identity)
{
    auto connection = connect();
    if (!connection) {
        return std::move(connection).error();
    }
    auto request = makeRequest("claim");
    putIdentity(request.get(), identity);
    auto reply = control(*connection, request);
    if (!reply) {
        xpc_connection_cancel(asConnection(*connection));
        return std::move(reply).error();
    }
    return std::unique_ptr<transport::ClaimedInterface>(
        std::make_unique<RemoteInterface>(std::move(*connection), getSelection(reply->get())));
}

bool RemoteTransport::startHotplug(transport::HotplugCallback callback)
{
    stopHotplug();
    auto state = std::make_shared<HotplugState>();
    state->callback = std::move(callback);
    auto connection = connect(^(xpc_object_t event) {
        if (xpc_get_type(event) == XPC_TYPE_DICTIONARY &&
            getString(event, "event") == "devices-changed") {
            std::lock_guard<std::mutex> lock(state->mutex);
            if (state->active && state->callback) {
                state->callback();
            }
        }
    });
    if (!connection) {
        return false;
    }
    auto reply = control(*connection, makeRequest("watch"));
    if (!reply || !getBool(reply->get(), "supported")) {
        xpc_connection_cancel(asConnection(*connection));
        return false;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    watch_ = std::move(*connection);
    hotplug_ = state;
    return true;
}

void RemoteTransport::stopHotplug()
{
    std::shared_ptr<HotplugState> state;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        state = std::move(hotplug_);
    }
    if (state) {
        std::lock_guard<std::mutex> lock(state->mutex);
        state->active = false;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (watch_) {
        xpc_connection_cancel(asConnection(watch_));
        watch_ = Xpc();
    }
}

} // namespace bifrost::service
