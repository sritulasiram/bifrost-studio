#include "bifrost/service/listener.hpp"

#include <memory>

namespace bifrost::service {

std::string signingRequirement(const std::vector<std::string>& identifiers,
                               const std::string& teamId)
{
    std::string any;
    for (const auto& identifier : identifiers) {
        if (!any.empty()) {
            any += " or ";
        }
        any += "identifier \"" + identifier + "\"";
    }
    if (identifiers.size() > 1) {
        any = "(" + any + ")";
    }
    if (teamId.empty()) {
        return any;
    }
    return "anchor apple generic and certificate leaf[subject.OU] = \"" + teamId +
           "\" and " + any;
}

Expected<std::unique_ptr<Listener>> Listener::start(Xpc connection,
                                                    const std::string& requirement,
                                                    PeerHandler handler)
{
    if (!connection) {
        return makeError(ErrorCode::serviceUnavailable, "Could not create an XPC listener");
    }
    auto* listener = static_cast<xpc_connection_t>(connection.get());
    if (!requirement.empty() &&
        xpc_connection_set_peer_code_signing_requirement(listener,
                                                         requirement.c_str()) != 0) {
        return makeError(ErrorCode::invalidArgument,
                         "Invalid code-signing requirement: " + requirement);
    }
    auto shared = std::make_shared<PeerHandler>(std::move(handler));
    xpc_connection_set_event_handler(listener, ^(xpc_object_t event) {
        if (xpc_get_type(event) == XPC_TYPE_CONNECTION) {
            (*shared)(static_cast<xpc_connection_t>(event));
        }
    });
    xpc_connection_resume(listener);
    std::unique_ptr<Listener> result(new Listener());
    result->connection_ = std::move(connection);
    return result;
}

Expected<std::unique_ptr<Listener>> Listener::machService(const std::string& name,
                                                          const std::string& requirement,
                                                          PeerHandler handler)
{
    auto connection = Xpc::adopt(xpc_connection_create_mach_service(
        name.c_str(), nullptr, XPC_CONNECTION_MACH_SERVICE_LISTENER));
    return start(std::move(connection), requirement, std::move(handler));
}

Expected<std::unique_ptr<Listener>> Listener::anonymous(PeerHandler handler,
                                                        const std::string& requirement)
{
    auto connection = Xpc::adopt(xpc_connection_create(nullptr, nullptr));
    return start(std::move(connection), requirement, std::move(handler));
}

Listener::~Listener()
{
    if (connection_) {
        xpc_connection_cancel(static_cast<xpc_connection_t>(connection_.get()));
    }
}

Xpc Listener::endpoint() const
{
    return Xpc::adopt(xpc_endpoint_create(static_cast<xpc_connection_t>(connection_.get())));
}

} // namespace bifrost::service
