// Helpers shared by the fuzz harnesses.
#ifndef BIFROST_FUZZ_COMMON_HPP
#define BIFROST_FUZZ_COMMON_HPP

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <unistd.h>

namespace fuzz {

// Rewrites one per-process scratch file with `size` bytes and returns its path.
// The core reads packages through a file descriptor, so harnesses go via disk.
inline std::string writeScratch(const std::string& suffix, const std::uint8_t* data,
                                std::size_t size)
{
    const char* base = std::getenv("TMPDIR");
    std::string path = std::string(base != nullptr ? base : "/tmp") + "/bifrost-fuzz-" +
                       std::to_string(::getpid()) + suffix;
    std::FILE* file = std::fopen(path.c_str(), "wb");
    if (file == nullptr) {
        std::abort();
    }
    if (size > 0 && std::fwrite(data, 1, size, file) != size) {
        std::abort();
    }
    std::fclose(file);
    return path;
}

} // namespace fuzz

#endif
