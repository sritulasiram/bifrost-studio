#include "capi/capi_internal.hpp"

#include "bifrost/transport/libusb_transport.hpp"

namespace bifrost::capi {

Expected<std::unique_ptr<transport::UsbTransport>> makeDefaultTransport()
{
    auto created = transport::LibusbTransport::create();
    if (!created) {
        return std::move(created).error();
    }
    return std::unique_ptr<transport::UsbTransport>(std::move(*created));
}

} // namespace bifrost::capi
