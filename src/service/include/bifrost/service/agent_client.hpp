// Client for the background agent (used by the CLI and tests; the SwiftUI
// app in milestone 5 speaks the same wire format).
#ifndef BIFROST_SERVICE_AGENT_CLIENT_HPP
#define BIFROST_SERVICE_AGENT_CLIENT_HPP

#include "bifrost/common/cancel.hpp"
#include "bifrost/service/wire.hpp"

#include <functional>
#include <mutex>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace bifrost::service {

struct AgentEvent {
    enum class Kind { started, progress, log, plan, finished, devicesChanged, disconnected };
    Kind kind{Kind::log};
    std::uint64_t job{};
    flash::Progress progress;
    LogLine log;
    std::optional<PlanSummary> plan;
    std::optional<JobResult> result;
};

// Called on an XPC queue; keep it short and thread-safe.
using AgentEventHandler = std::function<void(const AgentEvent&)>;

struct PackagePath {
    planner::Slot slot{planner::Slot::ap};
    std::string path;
};

struct FlashJobRequest {
    transport::UsbIdentity device;
    std::vector<PackagePath> packages;
    std::string typedModel;
    bool acceptDataErase{};
    bool reboot{true};
    std::vector<std::uint8_t> reviewedPit; // optional
};

class AgentClient {
public:
    // The agent's Mach service in the user's GUI domain (a LaunchAgent).
    static Expected<std::unique_ptr<AgentClient>> machService(const std::string& name,
                                                              const std::string& requirement,
                                                              AgentEventHandler handler = {});
    static Expected<std::unique_ptr<AgentClient>> endpoint(Xpc endpoint,
                                                           AgentEventHandler handler = {},
                                                           const std::string& requirement = {});
    // Disconnects. A running job keeps running in the agent. No event
    // handler call starts after the destructor returns.
    ~AgentClient();
    AgentClient(const AgentClient&) = delete;
    AgentClient& operator=(const AgentClient&) = delete;

    Expected<transport::UsbEnumeration> devices();
    // The current or most recent job, if any.
    Expected<std::optional<JobSnapshot>> status();
    // Starts event delivery to this client's handler.
    Status subscribe();

    // Opens each package file here and passes its descriptor to the agent.
    Expected<std::uint64_t> startFlash(const FlashJobRequest& request);
    Expected<std::uint64_t> startProbe(const transport::UsbIdentity& device, bool resume = false);
    Expected<std::uint64_t> startReadPit(const transport::UsbIdentity& device,
                                         bool resume = false);
    Status cancel(CancelLevel level);

private:
    AgentClient() = default;
    static Expected<std::unique_ptr<AgentClient>> open(Xpc connection,
                                                       const std::string& requirement,
                                                       AgentEventHandler handler);
    Expected<Xpc> request(const Xpc& message);
    Expected<std::uint64_t> startSimple(const char* kind, const transport::UsbIdentity& device,
                                        bool resume);

    struct HandlerState;
    Xpc connection_;
    std::shared_ptr<HandlerState> handler_;
};

} // namespace bifrost::service

#endif
