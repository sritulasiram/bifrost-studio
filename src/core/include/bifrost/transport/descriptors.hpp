#ifndef BIFROST_TRANSPORT_DESCRIPTORS_HPP
#define BIFROST_TRANSPORT_DESCRIPTORS_HPP

#include <cstdint>
#include <optional>
#include <vector>

namespace bifrost::transport {

enum class TransferType : std::uint8_t {
    control = 0,
    isochronous = 1,
    bulk = 2,
    interrupt = 3,
};

struct EndpointDescriptor {
    std::uint8_t address{};
    TransferType transferType{TransferType::control};
    std::uint16_t maxPacketSize{};
};

struct AlternateSetting {
    std::uint8_t interfaceNumber{};
    std::uint8_t alternateSetting{};
    std::uint8_t interfaceClass{};
    std::uint8_t interfaceSubClass{};
    std::uint8_t interfaceProtocol{};
    std::vector<EndpointDescriptor> endpoints;
};

struct OdinInterface {
    std::uint8_t interfaceNumber{};
    std::uint8_t alternateSetting{};
    std::uint8_t endpointIn{};
    std::uint8_t endpointOut{};
};

// Samsung Download Mode product IDs, as accepted by Heimdall
// (BridgeManager.h:76-78, BridgeManager.cpp:65-68): 0x6601, 0x685D, 0x68C3.
// A phone booted into Android (e.g. 0x6860, "SAMSUNG_Android") also exposes a
// CDC Data interface with bulk endpoints, so the interface shape alone does
// not identify Download Mode (hardware test, 2026-11-08).
bool isDownloadModeProductId(std::uint16_t productId) noexcept;

std::optional<OdinInterface> findOdinInterface(
    const std::vector<AlternateSetting>& alternates);

} // namespace bifrost::transport

#endif
