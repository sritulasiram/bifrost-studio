#include "bifrost/pit/pit.hpp"

#include <algorithm>
#include <string>
#include <unordered_set>

namespace bifrost::pit {
namespace {

constexpr std::uint32_t kPitMagic = 0x12349876;
constexpr std::size_t kHeaderSize = 28;
constexpr std::size_t kEntrySize = 132;
constexpr std::size_t kNameSize = 32;
constexpr std::uint32_t kMaximumEntries = 8192;
constexpr std::size_t kMaximumPitSize = 1024 * 1024;

std::uint32_t read32(const std::uint8_t* bytes)
{
    return static_cast<std::uint32_t>(bytes[0]) |
           (static_cast<std::uint32_t>(bytes[1]) << 8) |
           (static_cast<std::uint32_t>(bytes[2]) << 16) |
           (static_cast<std::uint32_t>(bytes[3]) << 24);
}

std::string readName(const std::uint8_t* bytes)
{
    const auto* end = std::find(bytes, bytes + kNameSize, std::uint8_t{0});
    return std::string(reinterpret_cast<const char*>(bytes),
                       static_cast<std::size_t>(end - bytes));
}

// FOTA names are informational and real PITs contain control characters in
// them (SM-N920I USERDATA: "remained\r\n"); keep them displayable.
std::string sanitized(std::string value)
{
    while (!value.empty() && (value.back() == '\r' || value.back() == '\n' || value.back() == ' ')) {
        value.pop_back();
    }
    for (auto& c : value) {
        const auto u = static_cast<unsigned char>(c);
        if (u < 0x20 || u > 0x7E) {
            c = '?';
        }
    }
    return value;
}

bool printable(const std::string& value)
{
    return std::all_of(value.begin(), value.end(), [](unsigned char c) {
        return c >= 0x20 && c <= 0x7E;
    });
}

Error pitError(const std::string& message)
{
    return makeError(ErrorCode::pitInvalid, "Invalid PIT: " + message);
}

} // namespace

std::size_t Table::countIdentifier(std::uint32_t identifier) const noexcept
{
    return static_cast<std::size_t>(
        std::count_if(entries.begin(), entries.end(),
                      [identifier](const Entry& e) { return e.identifier == identifier; }));
}

const Entry* Table::findByIdentifier(std::uint32_t identifier) const noexcept
{
    const auto found =
        std::find_if(entries.begin(), entries.end(), [identifier](const Entry& e) {
            return e.identifier == identifier;
        });
    return found == entries.end() ? nullptr : &*found;
}

const Entry* Table::findByName(const std::string& partitionName) const noexcept
{
    const auto found = std::find_if(
        entries.begin(), entries.end(),
        [&partitionName](const Entry& e) { return e.partitionName == partitionName; });
    return found == entries.end() ? nullptr : &*found;
}

Expected<Table> parse(const std::uint8_t* bytes, std::size_t size)
{
    if (bytes == nullptr || size < kHeaderSize) {
        return pitError("shorter than its 28-byte header");
    }
    if (size > kMaximumPitSize) {
        return pitError("larger than the 1 MiB limit");
    }
    if (read32(bytes) != kPitMagic) {
        return pitError("bad magic; expected 0x12349876");
    }
    const auto count = read32(bytes + 4);
    if (count == 0 || count > kMaximumEntries) {
        return pitError("entry count is zero or too large");
    }
    if (kHeaderSize + static_cast<std::size_t>(count) * kEntrySize > size) {
        return pitError("truncated before all declared entries");
    }
    // Data after the declared entries is ignored, as Heimdall does.
    Table table;
    table.entries.reserve(count);
    std::unordered_set<std::string> names;
    for (std::uint32_t index = 0; index < count; ++index) {
        const auto* e = bytes + kHeaderSize + static_cast<std::size_t>(index) * kEntrySize;
        Entry entry{read32(e),      read32(e + 4),  read32(e + 8),
                    read32(e + 12), read32(e + 16), read32(e + 20),
                    read32(e + 24), read32(e + 28), read32(e + 32),
                    readName(e + 36), readName(e + 68), sanitized(readName(e + 100))};
        if (entry.partitionName.empty() || !printable(entry.partitionName) ||
            !printable(entry.flashFilename)) {
            return pitError("entry " + std::to_string(index) +
                            " has an empty or invalid name");
        }
        // Identifiers may repeat: UFS devices number partitions per logical
        // unit (SM-N920I: CPEFS and BOTA0 are both 1). The planner refuses to
        // flash a partition whose identifier is shared, since the transfer
        // addresses partitions by identifier alone.
        // Names identify partitions in compare() and findByName(); a duplicate
        // would make those lookups ambiguous, so it is refused.
        if (!names.insert(entry.partitionName).second) {
            return pitError("duplicate partition name " + entry.partitionName);
        }
        table.entries.push_back(std::move(entry));
    }
    return table;
}

Comparison compare(const Table& device, const Table& firmware)
{
    Comparison result;
    for (const auto& theirs : firmware.entries) {
        const auto* ours = device.findByName(theirs.partitionName);
        if (ours == nullptr) {
            result.layoutDifferences.push_back(
                "partition " + theirs.partitionName +
                " exists in the firmware PIT but not on the device");
            continue;
        }
        if (ours->identifier != theirs.identifier ||
            ours->binaryType != theirs.binaryType ||
            ours->deviceType != theirs.deviceType) {
            result.layoutDifferences.push_back(
                "partition " + theirs.partitionName +
                " has a different identifier or type");
        } else if (ours->blockCount != theirs.blockCount) {
            result.sizeDifferences.push_back("partition " + theirs.partitionName +
                                             " has a different size");
        }
    }
    for (const auto& ours : device.entries) {
        if (firmware.findByName(ours.partitionName) == nullptr) {
            result.layoutDifferences.push_back(
                "partition " + ours.partitionName +
                " exists on the device but not in the firmware PIT");
        }
    }
    return result;
}

} // namespace bifrost::pit
