#ifndef BIFROST_SERVICE_LISTENER_HPP
#define BIFROST_SERVICE_LISTENER_HPP

#include "bifrost/common/error.hpp"
#include "bifrost/service/wire.hpp"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace bifrost::service {

// A code-signing requirement matching any of `identifiers`. With a team ID,
// the peer must also be signed by that Developer ID team; without one
// (development builds) only the signing identifier is checked, which any
// local code could imitate.
std::string signingRequirement(const std::vector<std::string>& identifiers,
                               const std::string& teamId);

// Accepts peer connections on a Mach service (launchd) or an anonymous
// endpoint (tests, in-process). The handler must set an event handler on the
// peer and resume it.
class Listener {
public:
    using PeerHandler = std::function<void(xpc_connection_t peer)>;

    // `requirement` (may be empty) is enforced on every incoming message.
    static Expected<std::unique_ptr<Listener>> machService(const std::string& name,
                                                           const std::string& requirement,
                                                           PeerHandler handler);
    static Expected<std::unique_ptr<Listener>> anonymous(PeerHandler handler,
                                                         const std::string& requirement = {});
    ~Listener();
    Listener(const Listener&) = delete;
    Listener& operator=(const Listener&) = delete;

    // Anonymous listeners only: an endpoint clients can connect to.
    Xpc endpoint() const;

private:
    Listener() = default;
    static Expected<std::unique_ptr<Listener>> start(Xpc connection,
                                                     const std::string& requirement,
                                                     PeerHandler handler);
    Xpc connection_;
};

} // namespace bifrost::service

#endif
