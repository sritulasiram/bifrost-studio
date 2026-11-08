#ifndef BIFROST_PROTOCOL_PACKET_HPP
#define BIFROST_PROTOCOL_PACKET_HPP

#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>

namespace bifrost::odin {

// Every Odin control request is a zero-filled 1024-byte packet:
// [command:u32le][request:u32le][arguments...] (Heimdall ControlPacket.h).
inline constexpr std::size_t kControlPacketSize = 1024;
inline constexpr std::size_t kMaximumArguments = (kControlPacketSize - 8) / 4;
using ControlPacket = std::array<std::uint8_t, kControlPacketSize>;

// Command families (Heimdall ControlPacket.h:35-38).
inline constexpr std::uint32_t kCommandSession = 0x64;
inline constexpr std::uint32_t kCommandPit = 0x65;
inline constexpr std::uint32_t kCommandFileTransfer = 0x66;
inline constexpr std::uint32_t kCommandEndSession = 0x67;

ControlPacket encodeControlPacket(std::uint32_t command, std::uint32_t request,
                                  std::initializer_list<std::uint32_t> arguments = {});

void writeLittleEndian32(std::uint8_t* destination, std::uint32_t value) noexcept;
std::uint32_t readLittleEndian32(const std::uint8_t* bytes) noexcept;

} // namespace bifrost::odin

#endif
