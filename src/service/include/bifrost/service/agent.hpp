// The per-user background agent (bifrost-agent). It hosts the core and runs
// at most one device job (flash, probe, read-pit) at a time, independently of
// any client: a UI that quits or crashes can reconnect and call "status" to
// reattach, and "subscribe" to resume live events.
//
// Requests (see wire.hpp for encoding):
//   hello                         -> version, role "agent"
//   devices                       -> enumeration
//   status                        -> job snapshot (if any job ran since start)
//   subscribe                     -> events on this connection:
//        started, progress, log, plan, finished, devices-changed
//   start {kind, device, resume, ...} -> job id; fails with illegalState if busy
//        resume: skip the handshake (probe and read-pit only; a flash with
//        resume is refused: it must be the first session of a Download Mode entry)
//        flash: packages [{slot, name, fd}], model, acceptErase, reboot,
//               optional reviewedPit (PIT bytes the user reviewed offline)
//   cancel {level}                -> CancelLevel 1 (before data) / 2 (after sequence)
//
// Firmware files arrive as file descriptors opened by the client, so the
// agent needs no folder permissions and no path can be swapped mid-flash.
#ifndef BIFROST_SERVICE_AGENT_HPP
#define BIFROST_SERVICE_AGENT_HPP

#include "bifrost/protocol/session.hpp"
#include "bifrost/transport/transport.hpp"

#include <xpc/xpc.h>

#include <cstddef>
#include <memory>

namespace bifrost::service {

struct AgentOptions {
    std::size_t logLimit{500};       // lines kept for reattaching clients
    unsigned int progressIntervalMs{50};
    odin::Timeouts timeouts{};
};

class Agent {
public:
    explicit Agent(std::shared_ptr<transport::UsbTransport> transport, AgentOptions options = {});
    ~Agent(); // shutdown(), then disconnects every client
    Agent(const Agent&) = delete;
    Agent& operator=(const Agent&) = delete;

    void accept(xpc_connection_t peer);
    bool jobRunning() const;

    // Used on SIGTERM: asks a running job to stop at the next safe point
    // (before data, or after the current sequence), waits for it, and
    // refuses new jobs.
    void shutdown();

private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
};

} // namespace bifrost::service

#endif
