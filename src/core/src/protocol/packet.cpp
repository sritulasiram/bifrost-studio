#include "bifrost/protocol/packet.hpp"

namespace bifrost::odin {

void writeLittleEndian32(std::uint8_t* destination, std::uint32_t value) noexcept
{
    destination[0] = static_cast<std::uint8_t>(value & 0xFF);
    destination[1] = static_cast<std::uint8_t>((value >> 8) & 0xFF);
    destination[2] = static_cast<std::uint8_t>((value >> 16) & 0xFF);
    destination[3] = static_cast<std::uint8_t>((value >> 24) & 0xFF);
}

std::uint32_t readLittleEndian32(const std::uint8_t* bytes) noexcept
{
    return static_cast<std::uint32_t>(bytes[0]) |
           (static_cast<std::uint32_t>(bytes[1]) << 8) |
           (static_cast<std::uint32_t>(bytes[2]) << 16) |
           (static_cast<std::uint32_t>(bytes[3]) << 24);
}

ControlPacket encodeControlPacket(std::uint32_t command, std::uint32_t request,
                                  std::initializer_list<std::uint32_t> arguments)
{
    ControlPacket packet{};
    writeLittleEndian32(packet.data(), command);
    writeLittleEndian32(packet.data() + 4, request);
    std::size_t offset = 8;
    for (const auto argument : arguments) {
        if (offset + 4 > packet.size()) {
            break; // kMaximumArguments is far above any real request
        }
        writeLittleEndian32(packet.data() + offset, argument);
        offset += 4;
    }
    return packet;
}

} // namespace bifrost::odin
