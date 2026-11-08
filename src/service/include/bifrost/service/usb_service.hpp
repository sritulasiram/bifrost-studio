// The privileged USB helper's request handling (bifrost-usbd).
//
// Scope is deliberately minimal: enumerate Samsung devices, claim the Odin
// interface, bulk read/write, zero-length transfers, release, and hot-plug
// notifications. It never opens files and never interprets the Odin
// protocol. Each peer connection holds at most one claimed interface, which
// is released when the peer disconnects or crashes.
#ifndef BIFROST_SERVICE_USB_SERVICE_HPP
#define BIFROST_SERVICE_USB_SERVICE_HPP

#include "bifrost/transport/transport.hpp"

#include <xpc/xpc.h>

#include <memory>

namespace bifrost::service {

class UsbService {
public:
    explicit UsbService(std::shared_ptr<transport::UsbTransport> transport);
    ~UsbService(); // cancels every peer; claimed interfaces are released
    UsbService(const UsbService&) = delete;
    UsbService& operator=(const UsbService&) = delete;

    // Takes a new peer connection from a Listener and starts serving it.
    void accept(xpc_connection_t peer);

private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
};

} // namespace bifrost::service

#endif
