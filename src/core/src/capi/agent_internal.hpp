// Test seam for the agent C API (not installed).
#ifndef BIFROST_CAPI_AGENT_INTERNAL_HPP
#define BIFROST_CAPI_AGENT_INTERNAL_HPP

#include "bifrost/bifrost.h"
#include "bifrost/service/wire.hpp"

namespace bifrost::capi {

// Connects to an agent at an in-process endpoint (tests).
bf_agent* connectAgentEndpoint(service::Xpc endpoint, bf_agent_event_fn fn, void* user,
                               bf_error* error);

} // namespace bifrost::capi

#endif
