// Internal seams of the C API (not installed, not exported).
#ifndef BIFROST_CAPI_INTERNAL_HPP
#define BIFROST_CAPI_INTERNAL_HPP

#include "bifrost/bifrost.h"
#include "bifrost/common/error.hpp"
#include "bifrost/transport/transport.hpp"

#include <memory>

namespace bifrost::capi {

// The transport bf_create() uses. Linked from exactly one of
// default_transport_libusb.cpp or default_transport_none.cpp. A null
// transport (built without USB) makes device functions fail with
// BF_E_UNSUPPORTED.
Expected<std::unique_ptr<transport::UsbTransport>> makeDefaultTransport();

// Creates a context over any transport (tests use the simulator).
bf_ctx* createContext(std::unique_ptr<transport::UsbTransport> transport);

} // namespace bifrost::capi

#endif
