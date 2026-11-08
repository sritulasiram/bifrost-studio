// bifrost-usbd: the privileged USB helper (LaunchDaemon, root). Serves only
// the background agent, verified by code signature on every message.
#include "bifrost/service/listener.hpp"
#include "bifrost/service/usb_service.hpp"
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
#include <unistd.h>

using namespace bifrost;
using namespace bifrost::service;

int main(int argc, char** argv)
{
    if (argc > 1 && std::strcmp(argv[1], "--version") == 0) {
        std::printf("bifrost-usbd %s\n", bifrost_core_version());
        return 0;
    }
    os_log_t log = os_log_create(config::kUsbdService, "usbd");
    std::signal(SIGPIPE, SIG_IGN);
    if (::geteuid() != 0) {
        os_log_error(log, "not running as root; detaching the CDC driver may fail");
    }
    if (config::kTeamId[0] == '\0') {
        os_log_error(log, "development build: the agent is checked by signing identifier only");
    }

    auto usb = transport::LibusbTransport::create();
    if (!usb) {
        os_log_error(log, "libusb init failed: %{public}s", usb.error().message.c_str());
        return 1;
    }
    std::shared_ptr<transport::UsbTransport> transport(std::move(*usb));
    auto service = std::make_shared<UsbService>(transport);
    const auto requirement = signingRequirement({config::kAgentIdentifier}, config::kTeamId);
    auto listener = Listener::machService(config::kUsbdService, requirement,
                                          [service](xpc_connection_t peer) {
                                              service->accept(peer);
                                          });
    if (!listener) {
        os_log_error(log, "listener failed: %{public}s", listener.error().message.c_str());
        return 1;
    }

    std::signal(SIGTERM, SIG_IGN);
    dispatch_source_t term = dispatch_source_create(DISPATCH_SOURCE_TYPE_SIGNAL, SIGTERM, 0,
                                                    dispatch_get_main_queue());
    dispatch_source_set_event_handler(term, ^{
        os_log(log, "SIGTERM: exiting; claimed interfaces are released by the OS");
        std::exit(0);
    });
    dispatch_resume(term);
    os_log(log, "bifrost-usbd %{public}s ready", bifrost_core_version());
    dispatch_main();
}
