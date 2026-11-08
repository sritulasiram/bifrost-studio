#ifndef BIFROST_PIT_PIT_HPP
#define BIFROST_PIT_PIT_HPP

#include "bifrost/common/error.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace bifrost::pit {

inline constexpr std::uint32_t kBinaryApplicationProcessor = 0;
inline constexpr std::uint32_t kBinaryCommunicationProcessor = 1;

// Field layout follows Heimdall libpit (libpit.cpp:97-151).
struct Entry {
    std::uint32_t binaryType{};
    std::uint32_t deviceType{};
    std::uint32_t identifier{};
    std::uint32_t attributes{};
    std::uint32_t updateAttributes{};
    std::uint32_t blockSizeOrOffset{};
    std::uint32_t blockCount{};
    std::uint32_t fileOffset{};
    std::uint32_t fileSize{};
    std::string partitionName;
    std::string flashFilename;
    std::string fotaFilename;
};

struct Table {
    std::vector<Entry> entries;

    // First entry with this identifier. Identifiers can repeat across UFS
    // logical units; use countIdentifier() before relying on it.
    const Entry* findByIdentifier(std::uint32_t identifier) const noexcept;
    std::size_t countIdentifier(std::uint32_t identifier) const noexcept;
    const Entry* findByName(const std::string& partitionName) const noexcept;
};

Expected<Table> parse(const std::uint8_t* bytes, std::size_t size);

// Compares a PIT shipped in firmware with the device's PIT. Different
// partition names or identifiers indicate firmware for a different device
// layout; block-count changes alone may be a legitimate PIT revision.
struct Comparison {
    std::vector<std::string> layoutDifferences; // names/ids/types differ
    std::vector<std::string> sizeDifferences;   // block counts differ
    bool sameLayout() const noexcept { return layoutDifferences.empty(); }
};

Comparison compare(const Table& device, const Table& firmware);

} // namespace bifrost::pit

#endif
