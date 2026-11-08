#include "capi/capi_internal.hpp"

namespace bifrost::capi {

Expected<std::unique_ptr<transport::UsbTransport>> makeDefaultTransport()
{
    return std::unique_ptr<transport::UsbTransport>();
}

} // namespace bifrost::capi
