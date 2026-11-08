// LZ4 frame validation and streaming decode of a TAR entry.
#include "bifrost/firmware/lz4.hpp"
#include "bifrost/firmware/package.hpp"
#include "fuzz_common.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <memory>
#include <vector>

namespace {

// Minimal ustar wrapper: one regular file "image.img.lz4".
std::vector<std::uint8_t> wrap(const std::uint8_t* data, std::size_t size)
{
    std::array<std::uint8_t, 512> header{};
    std::memcpy(header.data(), "image.img.lz4", 13);
    std::snprintf(reinterpret_cast<char*>(header.data() + 100), 8, "%07o", 0644);
    std::snprintf(reinterpret_cast<char*>(header.data() + 108), 8, "%07o", 0);
    std::snprintf(reinterpret_cast<char*>(header.data() + 116), 8, "%07o", 0);
    std::snprintf(reinterpret_cast<char*>(header.data() + 124), 12, "%011llo",
                  static_cast<unsigned long long>(size));
    std::snprintf(reinterpret_cast<char*>(header.data() + 136), 12, "%011o", 0);
    std::memset(header.data() + 148, ' ', 8);
    header[156] = '0';
    std::memcpy(header.data() + 257, "ustar", 6);
    header[263] = '0';
    header[264] = '0';
    unsigned sum = 0;
    for (auto b : header) {
        sum += b;
    }
    std::snprintf(reinterpret_cast<char*>(header.data() + 148), 8, "%06o", sum);
    header[155] = ' ';
    std::vector<std::uint8_t> tar(header.begin(), header.end());
    tar.insert(tar.end(), data, data + size);
    tar.resize(tar.size() + (512 - size % 512) % 512 + 1024, 0);
    return tar;
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size)
{
    if (size == 0) {
        return 0;
    }
    const auto tar = wrap(data, size);
    const auto path = fuzz::writeScratch(".tar", tar.data(), tar.size());
    auto package = bifrost::firmware::inspectPackage(path);
    if (!package || package->entries.size() != 1 || !package->entries[0].lz4Compressed) {
        __builtin_trap(); // the wrapper always yields one LZ4 entry
    }
    auto shared = std::make_shared<const bifrost::firmware::Package>(std::move(*package));
    const auto& entry = shared->entries[0];
    auto decoded = bifrost::firmware::validateLz4(*shared, entry);
    if (!decoded) {
        return 0;
    }
    auto reader = bifrost::firmware::Lz4StreamReader::open(shared, entry, *decoded);
    if (!reader) {
        __builtin_trap(); // a validated frame must open
    }
    std::vector<std::uint8_t> buffer(70001);
    std::uint64_t offset = 0;
    while (offset < *decoded) {
        const auto n = static_cast<std::size_t>(
            std::min<std::uint64_t>(buffer.size(), *decoded - offset));
        if (!(*reader)->read(offset, buffer.data(), n)) {
            __builtin_trap(); // a validated frame must stream to its full size
        }
        offset += n;
    }
    return 0;
}
