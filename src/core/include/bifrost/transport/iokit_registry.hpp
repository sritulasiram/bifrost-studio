#ifndef BIFROST_TRANSPORT_IOKIT_REGISTRY_HPP
#define BIFROST_TRANSPORT_IOKIT_REGISTRY_HPP

#include "bifrost/common/error.hpp"

#include <cstddef>

namespace bifrost::transport {

// Read-only diagnostic: number of Samsung (VID 04e8) devices in the IOKit
// registry. Never claims anything.
Expected<std::size_t> countSamsungDevicesInIokitRegistry();

} // namespace bifrost::transport

#endif
