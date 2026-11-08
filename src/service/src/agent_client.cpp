#include "bifrost/service/agent_client.hpp"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>

namespace bifrost::service {
namespace {

xpc_connection_t asConnection(const Xpc& x)
{
    return static_cast<xpc_connection_t>(x.get());
}

std::optional<AgentEvent> decodeEvent(xpc_object_t message)
{
    const auto name = getString(message, "event");
    AgentEvent event;
    event.job = getUint(message, "job").value_or(0);
    if (name == "started") {
        event.kind = AgentEvent::Kind::started;
    } else if (name == "progress") {
        event.kind = AgentEvent::Kind::progress;
        if (auto p = xpc_dictionary_get_dictionary(message, "progress")) {
            event.progress = decodeProgress(p);
        }
    } else if (name == "log") {
        event.kind = AgentEvent::Kind::log;
        const auto level = std::min<std::uint64_t>(getUint(message, "level").value_or(0), 2);
        event.log = {static_cast<flash::LogLevel>(level), getString(message, "message")};
    } else if (name == "plan") {
        event.kind = AgentEvent::Kind::plan;
        if (auto p = xpc_dictionary_get_dictionary(message, "plan")) {
            event.plan = decodePlan(p);
        }
    } else if (name == "finished") {
        event.kind = AgentEvent::Kind::finished;
        if (auto r = xpc_dictionary_get_dictionary(message, "result")) {
            event.result = decodeResult(r);
        }
    } else if (name == "devices-changed") {
        event.kind = AgentEvent::Kind::devicesChanged;
    } else {
        return std::nullopt;
    }
    return event;
}

std::string baseName(const std::string& path)
{
    const auto slash = path.find_last_of('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

} // namespace

// Shared with the connection's event block. XPC can deliver a final event
// after the client is destroyed; `active` (checked under the mutex the
// callback runs under) makes that a no-op.
struct AgentClient::HandlerState {
    std::mutex mutex;
    bool active{true};
    AgentEventHandler handler;

    void deliver(const AgentEvent& event)
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (active && handler) {
            handler(event);
        }
    }
};

Expected<std::unique_ptr<AgentClient>> AgentClient::open(Xpc connection,
                                                         const std::string& requirement,
                                                         AgentEventHandler handler)
{
    if (!connection) {
        return makeError(ErrorCode::serviceUnavailable, "Could not create an agent connection");
    }
    if (!requirement.empty() &&
        xpc_connection_set_peer_code_signing_requirement(asConnection(connection),
                                                         requirement.c_str()) != 0) {
        return makeError(ErrorCode::invalidArgument, "Invalid agent requirement");
    }
    auto state = std::make_shared<HandlerState>();
    state->handler = std::move(handler);
    xpc_connection_set_event_handler(asConnection(connection), ^(xpc_object_t message) {
        if (xpc_get_type(message) == XPC_TYPE_ERROR) {
            AgentEvent event;
            event.kind = AgentEvent::Kind::disconnected;
            state->deliver(event);
            return;
        }
        if (xpc_get_type(message) == XPC_TYPE_DICTIONARY) {
            if (auto event = decodeEvent(message)) {
                state->deliver(*event);
            }
        }
    });
    xpc_connection_resume(asConnection(connection));

    std::unique_ptr<AgentClient> client(new AgentClient());
    client->connection_ = std::move(connection);
    client->handler_ = state;
    auto hello = client->request(makeRequest("hello"));
    if (!hello) {
        return std::move(hello).error();
    }
    if (getUint(hello->get(), "version").value_or(0) != kWireVersion ||
        getString(hello->get(), "role") != "agent") {
        return makeError(ErrorCode::serviceUnavailable,
                         "The background agent is a different version; reinstall Bifrost Studio");
    }
    return client;
}

Expected<std::unique_ptr<AgentClient>> AgentClient::machService(const std::string& name,
                                                                const std::string& requirement,
                                                                AgentEventHandler handler)
{
    return open(Xpc::adopt(xpc_connection_create_mach_service(name.c_str(), nullptr, 0)),
                requirement, std::move(handler));
}

Expected<std::unique_ptr<AgentClient>> AgentClient::endpoint(Xpc endpoint,
                                                             AgentEventHandler handler,
                                                             const std::string& requirement)
{
    return open(Xpc::adopt(xpc_connection_create_from_endpoint(
                    static_cast<xpc_endpoint_t>(endpoint.get()))),
                requirement, std::move(handler));
}

AgentClient::~AgentClient()
{
    if (handler_) {
        std::lock_guard<std::mutex> lock(handler_->mutex);
        handler_->active = false;
    }
    if (connection_) {
        xpc_connection_cancel(asConnection(connection_));
    }
}

Expected<Xpc> AgentClient::request(const Xpc& message)
{
    return call(asConnection(connection_), message.get(), "background agent",
                ErrorCode::serviceUnavailable, ErrorCode::serviceUnavailable);
}

Expected<transport::UsbEnumeration> AgentClient::devices()
{
    auto reply = request(makeRequest("devices"));
    if (!reply) {
        return std::move(reply).error();
    }
    xpc_object_t encoded = xpc_dictionary_get_dictionary(reply->get(), "enumeration");
    if (encoded == nullptr) {
        return makeError(ErrorCode::protocolViolation, "Agent sent no device list");
    }
    return decodeEnumeration(encoded);
}

Expected<std::optional<JobSnapshot>> AgentClient::status()
{
    auto reply = request(makeRequest("status"));
    if (!reply) {
        return std::move(reply).error();
    }
    std::optional<JobSnapshot> snapshot;
    if (auto job = xpc_dictionary_get_dictionary(reply->get(), "job")) {
        snapshot = decodeSnapshot(job);
    }
    return snapshot;
}

Status AgentClient::subscribe()
{
    auto reply = request(makeRequest("subscribe"));
    if (!reply) {
        return std::move(reply).error();
    }
    return success();
}

Expected<std::uint64_t> AgentClient::startSimple(const char* kind,
                                                 const transport::UsbIdentity& device,
                                                 bool resume)
{
    auto message = makeRequest("start");
    xpc_dictionary_set_string(message.get(), "kind", kind);
    xpc_dictionary_set_bool(message.get(), "resume", resume);
    putIdentity(message.get(), device);
    auto reply = request(message);
    if (!reply) {
        return std::move(reply).error();
    }
    return getUint(reply->get(), "job").value_or(0);
}

Expected<std::uint64_t> AgentClient::startProbe(const transport::UsbIdentity& device,
                                                bool resume)
{
    return startSimple(toString(JobKind::probe), device, resume);
}

Expected<std::uint64_t> AgentClient::startReadPit(const transport::UsbIdentity& device,
                                                  bool resume)
{
    return startSimple(toString(JobKind::readPit), device, resume);
}

Expected<std::uint64_t> AgentClient::startFlash(const FlashJobRequest& flashRequest)
{
    auto message = makeRequest("start");
    xpc_dictionary_set_string(message.get(), "kind", toString(JobKind::flash));
    putIdentity(message.get(), flashRequest.device);
    xpc_dictionary_set_string(message.get(), "model", flashRequest.typedModel.c_str());
    xpc_dictionary_set_bool(message.get(), "acceptErase", flashRequest.acceptDataErase);
    xpc_dictionary_set_bool(message.get(), "reboot", flashRequest.reboot);
    if (!flashRequest.reviewedPit.empty()) {
        putData(message.get(), "reviewedPit", flashRequest.reviewedPit.data(),
                flashRequest.reviewedPit.size());
    }
    auto packages = Xpc::adopt(xpc_array_create(nullptr, 0));
    for (const auto& package : flashRequest.packages) {
        const int fd = ::open(package.path.c_str(), O_RDONLY | O_CLOEXEC);
        if (fd < 0) {
            return makeError(ErrorCode::ioError,
                             "Could not open " + package.path + ": " + std::strerror(errno));
        }
        auto item = Xpc::adopt(xpc_dictionary_create(nullptr, nullptr, 0));
        xpc_dictionary_set_uint64(item.get(), "slot", static_cast<std::uint64_t>(package.slot));
        xpc_dictionary_set_string(item.get(), "name", baseName(package.path).c_str());
        xpc_dictionary_set_fd(item.get(), "fd", fd); // XPC duplicates the descriptor
        ::close(fd);
        xpc_array_append_value(packages.get(), item.get());
    }
    xpc_dictionary_set_value(message.get(), "packages", packages.get());
    auto reply = request(message);
    if (!reply) {
        return std::move(reply).error();
    }
    return getUint(reply->get(), "job").value_or(0);
}

Status AgentClient::cancel(CancelLevel level)
{
    auto message = makeRequest("cancel");
    xpc_dictionary_set_uint64(message.get(), "level", static_cast<std::uint64_t>(level));
    auto reply = request(message);
    if (!reply) {
        return std::move(reply).error();
    }
    return success();
}

} // namespace bifrost::service
