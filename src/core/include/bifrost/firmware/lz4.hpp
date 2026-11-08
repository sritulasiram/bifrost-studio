#ifndef BIFROST_FIRMWARE_LZ4_HPP
#define BIFROST_FIRMWARE_LZ4_HPP

#include "bifrost/common/cancel.hpp"
#include "bifrost/common/error.hpp"
#include "bifrost/firmware/package.hpp"

#include <cstdint>
#include <memory>
#include <vector>

struct LZ4F_dctx_s;

namespace bifrost::firmware {

// Fully decodes an LZ4-frame TAR entry without storing the output, checking
// frame integrity (including any content checksum and declared size). Returns
// the exact decompressed size. Run before any device access so a corrupt
// image is rejected before data is sent.
Expected<std::uint64_t> validateLz4(const Package& package,
                                    const ArchiveEntry& entry,
                                    const ByteProgress& progress = {},
                                    const CancelToken* cancel = nullptr);

// Sequential decoder used during transfer: reads must be contiguous and
// start at offset 0. Memory use is bounded (two 1 MiB buffers).
class Lz4StreamReader {
public:
    static Expected<std::unique_ptr<Lz4StreamReader>> open(
        std::shared_ptr<const Package> package, ArchiveEntry entry,
        std::uint64_t decodedSize);
    ~Lz4StreamReader();
    Lz4StreamReader(const Lz4StreamReader&) = delete;
    Lz4StreamReader& operator=(const Lz4StreamReader&) = delete;

    Status read(std::uint64_t offset, std::uint8_t* destination, std::size_t size);

private:
    Lz4StreamReader(std::shared_ptr<const Package> package, ArchiveEntry entry,
                    std::uint64_t decodedSize, LZ4F_dctx_s* context);
    Status refill();

    std::shared_ptr<const Package> package_;
    ArchiveEntry entry_;
    std::uint64_t decodedSize_{};
    LZ4F_dctx_s* context_{};
    std::vector<std::uint8_t> input_;
    std::size_t inputOffset_{};
    std::size_t inputAvailable_{};
    std::uint64_t consumed_{};
    std::vector<std::uint8_t> output_;
    std::size_t outputOffset_{};
    std::size_t outputAvailable_{};
    std::uint64_t position_{};
    bool frameEnded_{};
};

} // namespace bifrost::firmware

#endif
