#ifndef BIFROST_PROTOCOL_STATUS_TEXT_HPP
#define BIFROST_PROTOCOL_STATUS_TEXT_HPP

// Internal helpers shared by session.cpp and file_transfer.cpp.

#include "bifrost/common/error.hpp"

#include <cstdint>
#include <string>

namespace bifrost::odin::detail {

inline constexpr std::uint32_t kBootloaderFail = 0xFFFFFFFF;

// Labels for negative statuses as named by Samsung-Loki/Thor (Extensions.cs).
// Every negative status is a failure regardless of label.
inline std::string describeStatus(std::int32_t status)
{
    switch (status) {
    case -2: return " (write-protect)";
    case -3: return " (erase failure)";
    case -4: return " (write failure)";
    case -5: return " (authentication failure)";
    case -6: return " (size failure)";
    case -7: return " (ext4 failure)";
    default: return "";
    }
}

inline Error rejected(const std::string& what, std::uint32_t value)
{
    const auto status = static_cast<std::int32_t>(value);
    return makeError(ErrorCode::bootloaderRejected,
                     "Bootloader rejected " + what + " with status " +
                         std::to_string(status) + describeStatus(status));
}

inline bool negative(std::uint32_t value)
{
    return static_cast<std::int32_t>(value) < 0;
}

} // namespace bifrost::odin::detail

#endif
