// bifrost-agent: the per-user background agent (LaunchAgent). Hosts the core
// and runs flash jobs independently of the UI.
//
//   --direct-usb   use libusb in this process instead of the root helper
//                  (for when hardware test T2 shows root is not required)
#include "bifrost/service/agent.hpp"
#include "bifrost/service/listener.hpp"
#include "bifrost/service/remote_transport.hpp"
#include "bifrost/transport/libusb_transport.hpp"
#include "bifrost/version.h"
#include "service_config.hpp"

#include <dispatch/dispatch.h>
#include <os/log.h>

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>

using namespace bifrost;
using namespace bifrost::service;

int main(int argc, char** argv)
{
    bool directUsb = false;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--version") == 0) {
            std::printf("bifrost-agent %s\n", bifrost_core_version());
            return 0;
        }
        if (std::strcmp(argv[i], "--direct-usb") == 0) {
            directUsb = true;
        }
    }
    os_log_t log = os_log_create(config::kAgentService, "agent");
    std::signal(SIGPIPE, SIG_IGN);
    if (config::kTeamId[0] == '\0') {
        os_log_error(log, "development build: peers are checked by signing identifier only");
    }

    std::shared_ptr<transport::UsbTransport> transport;
    if (directUsb) {
        auto usb = transport::LibusbTransport::create();
        if (!usb) {
            os_log_error(log, "libusb init failed: %{public}s", usb.error().message.c_str());
            return 1;
        }
        transport = std::move(*usb);
    } else {
        transport = RemoteTransport::machService(
            config::kUsbdService, signingRequirement({config::kUsbdIdentifier}, config::kTeamId));
    }

    auto agent = std::make_shared<Agent>(transport);
    const auto requirement =
        signingRequirement({config::kAppIdentifier, config::kCliIdentifier}, config::kTeamId);
    auto listener = Listener::machService(config::kAgentService, requirement,
                                          [agent](xpc_connection_t peer) { agent->accept(peer); });
    if (!listener) {
        os_log_error(log, "listener failed: %{public}s", listener.error().message.c_str());
        return 1;
    }

    // SIGTERM (logout, unregister): stop a running flash at the next safe
    // point, wait for it, then exit. launchd's ExitTimeOut bounds the wait.
    std::signal(SIGTERM, SIG_IGN);
    dispatch_queue_t termQueue = dispatch_queue_create("bifrost.agent.term", nullptr);
    dispatch_source_t term =
        dispatch_source_create(DISPATCH_SOURCE_TYPE_SIGNAL, SIGTERM, 0, termQueue);
    dispatch_source_set_event_handler(term, ^{
        os_log(log, "SIGTERM: stopping at the next safe point");
        agent->shutdown();
        std::exit(0);
    });
    dispatch_resume(term);
    os_log(log, "bifrost-agent %{public}s ready (%{public}s)", bifrost_core_version(),
           directUsb ? "direct USB" : "via USB helper");
    dispatch_main();
}
