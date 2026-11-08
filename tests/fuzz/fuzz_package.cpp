// .tar.md5 trailer finder and TAR parser. The first byte selects the mode:
//   0: plain .tar (TAR parser only)
//   1: .tar.md5 as given (trailer finder; checksum almost always fails)
//   2: .tar.md5 with a valid trailer appended (TAR parser behind the MD5 gate)
#include "bifrost/firmware/package.hpp"
#include "fuzz_common.hpp"

#include <CommonCrypto/CommonDigest.h>

#include <array>
#include <vector>

#pragma clang diagnostic ignored "-Wdeprecated-declarations"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size)
{
    if (size < 1) {
        return 0;
    }
    const auto mode = data[0] % 3;
    std::vector<std::uint8_t> bytes(data + 1, data + size);
    std::string suffix = mode == 0 ? ".tar" : ".tar.md5";
    if (mode == 2) {
        std::array<std::uint8_t, CC_MD5_DIGEST_LENGTH> digest{};
        CC_MD5(bytes.data(), static_cast<CC_LONG>(bytes.size()), digest.data());
        static const char hex[] = "0123456789abcdef";
        std::string trailer;
        for (auto b : digest) {
            trailer.push_back(hex[b >> 4]);
            trailer.push_back(hex[b & 15]);
        }
        trailer += "  fuzz.tar\n";
        bytes.insert(bytes.end(), trailer.begin(), trailer.end());
    }
    const auto path = fuzz::writeScratch(suffix, bytes.data(), bytes.size());
    auto package = bifrost::firmware::inspectPackage(path);
    if (package) {
        std::array<std::uint8_t, 64> buffer{};
        for (const auto& entry : package->entries) {
            if (entry.dataOffset + entry.size > package->archiveSize) {
                __builtin_trap(); // entries must lie inside the archive
            }
            const auto n = static_cast<std::size_t>(
                std::min<std::uint64_t>(entry.size, buffer.size()));
            if (n > 0 && !bifrost::firmware::readEntryBytes(*package, entry, 0,
                                                           buffer.data(), n)) {
                __builtin_trap(); // a parsed entry must be readable
            }
        }
    }
    return 0;
}
