#include "bifrost/transport/descriptors.hpp"

namespace bifrost::transport {
namespace {

constexpr std::uint8_t kCdcDataClass = 0x0A;
constexpr std::uint8_t kEndpointDirectionIn = 0x80;

} // namespace

bool isDownloadModeProductId(std::uint16_t productId) noexcept
{
    return productId == 0x6601 || productId == 0x685D || productId == 0x68C3;
}

std::optional<OdinInterface> findOdinInterface(
    const std::vector<AlternateSetting>& alternates)
{
    for (const auto& alternate : alternates) {
        if (alternate.interfaceClass != kCdcDataClass ||
            alternate.endpoints.size() != 2) {
            continue;
        }

        const EndpointDescriptor* endpointIn = nullptr;
        const EndpointDescriptor* endpointOut = nullptr;
        for (const auto& endpoint : alternate.endpoints) {
            if (endpoint.transferType != TransferType::bulk) {
                endpointIn = nullptr;
                endpointOut = nullptr;
                break;
            }

            const bool isIn = (endpoint.address & kEndpointDirectionIn) != 0;
            auto*& selected = isIn ? endpointIn : endpointOut;
            if (selected != nullptr) {
                endpointIn = nullptr;
                endpointOut = nullptr;
                break;
            }
            selected = &endpoint;
        }

        if (endpointIn != nullptr && endpointOut != nullptr) {
            return OdinInterface{
                alternate.interfaceNumber,
                alternate.alternateSetting,
                endpointIn->address,
                endpointOut->address,
            };
        }
    }

    return std::nullopt;
}

} // namespace bifrost::transport
