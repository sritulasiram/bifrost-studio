#include "bifrost/firmware/lz4.hpp"

#include <lz4frame.h>

#include <algorithm>
#include <cstring>
#include <limits>
#include <optional>

namespace bifrost::firmware {
namespace {

constexpr std::size_t kBufferSize = 1024 * 1024;

Error lz4Error(const ArchiveEntry& entry, const std::string& message)
{
    return makeError(ErrorCode::lz4Invalid, entry.name + ": " + message);
}

struct ContextDeleter {
    void operator()(LZ4F_dctx* context) const noexcept
    {
        LZ4F_freeDecompressionContext(context);
    }
};

// Shared decode loop. `sink` receives decoded bytes; returns the total.
template <typename Sink>
Expected<std::uint64_t> decodeAll(const Package& package, const ArchiveEntry& entry,
                                  const ByteProgress& progress,
                                  const CancelToken* cancel, Sink&& sink)
{
    LZ4F_dctx* raw = nullptr;
    if (LZ4F_isError(LZ4F_createDecompressionContext(&raw, LZ4F_VERSION))) {
        return lz4Error(entry, "could not initialize the LZ4 decoder");
    }
    std::unique_ptr<LZ4F_dctx, ContextDeleter> context(raw);
    std::vector<std::uint8_t> input(kBufferSize);
    std::vector<std::uint8_t> output(kBufferSize);
    std::uint64_t consumed = 0;
    std::uint64_t produced = 0;
    std::size_t offset = 0;
    std::size_t available = 0;
    std::optional<std::uint64_t> declared;
    bool headerRead = false;

    for (;;) {
        if (cancel != nullptr && cancel->honoured(false)) {
            return makeError(ErrorCode::cancelled, "Image check cancelled");
        }
        if (available == 0) {
            if (consumed == entry.size) {
                return lz4Error(entry, "LZ4 frame is truncated");
            }
            const auto amount = static_cast<std::size_t>(
                std::min<std::uint64_t>(input.size(), entry.size - consumed));
            if (auto read = readEntryBytes(package, entry, consumed, input.data(), amount);
                !read) {
                return read.error();
            }
            consumed += amount;
            offset = 0;
            available = amount;
        }
        if (!headerRead) {
            LZ4F_frameInfo_t info{};
            std::size_t headerSize = available;
            const auto result =
                LZ4F_getFrameInfo(context.get(), &info, input.data() + offset, &headerSize);
            if (LZ4F_isError(result)) {
                return lz4Error(entry, std::string("invalid LZ4 frame header: ") +
                                           LZ4F_getErrorName(result));
            }
            if (info.contentSize != 0) {
                declared = info.contentSize;
            }
            offset += headerSize;
            available -= headerSize;
            headerRead = true;
            continue;
        }
        std::size_t sourceSize = available;
        std::size_t destinationSize = output.size();
        const auto hint = LZ4F_decompress(context.get(), output.data(), &destinationSize,
                                          input.data() + offset, &sourceSize, nullptr);
        if (LZ4F_isError(hint)) {
            return lz4Error(entry, LZ4F_getErrorName(hint));
        }
        offset += sourceSize;
        available -= sourceSize;
        if (destinationSize > 0) {
            if (destinationSize > std::numeric_limits<std::uint64_t>::max() - produced ||
                (declared && produced + destinationSize > *declared)) {
                return lz4Error(entry, "output exceeds the declared content size");
            }
            if (auto sunk = sink(output.data(), destinationSize); !sunk) {
                return sunk.error();
            }
            produced += destinationSize;
            if (progress) {
                progress(consumed - available, entry.size);
            }
        }
        if (hint == 0) {
            if (available != 0 || consumed != entry.size) {
                return lz4Error(entry, "TAR entry contains data after its LZ4 frame");
            }
            break;
        }
        if (sourceSize == 0 && destinationSize == 0 && available > 0) {
            return lz4Error(entry, "LZ4 decoder made no progress");
        }
    }
    if (produced == 0) {
        return lz4Error(entry, "decompressed image is empty");
    }
    if (declared && produced != *declared) {
        return lz4Error(entry, "output size differs from the declared content size");
    }
    return produced;
}

} // namespace

Expected<std::uint64_t> validateLz4(const Package& package, const ArchiveEntry& entry,
                                    const ByteProgress& progress,
                                    const CancelToken* cancel)
{
    return decodeAll(package, entry, progress, cancel,
                     [](const std::uint8_t*, std::size_t) { return success(); });
}

Lz4StreamReader::Lz4StreamReader(std::shared_ptr<const Package> package,
                                 ArchiveEntry entry, std::uint64_t decodedSize,
                                 LZ4F_dctx_s* context)
    : package_(std::move(package)), entry_(std::move(entry)),
      decodedSize_(decodedSize), context_(context), input_(kBufferSize),
      output_(kBufferSize)
{
}

Lz4StreamReader::~Lz4StreamReader()
{
    LZ4F_freeDecompressionContext(context_);
}

Expected<std::unique_ptr<Lz4StreamReader>> Lz4StreamReader::open(
    std::shared_ptr<const Package> package, ArchiveEntry entry,
    std::uint64_t decodedSize)
{
    LZ4F_dctx* context = nullptr;
    if (!package ||
        LZ4F_isError(LZ4F_createDecompressionContext(&context, LZ4F_VERSION))) {
        return lz4Error(entry, "could not initialize the LZ4 decoder");
    }
    return std::unique_ptr<Lz4StreamReader>(
        new Lz4StreamReader(std::move(package), std::move(entry), decodedSize, context));
}

Status Lz4StreamReader::refill()
{
    while (outputAvailable_ == 0) {
        if (frameEnded_) {
            return lz4Error(entry_, "read past the end of the decoded image");
        }
        if (inputAvailable_ == 0) {
            if (consumed_ == entry_.size) {
                return lz4Error(entry_, "LZ4 frame is truncated");
            }
            const auto amount = static_cast<std::size_t>(
                std::min<std::uint64_t>(input_.size(), entry_.size - consumed_));
            if (auto read =
                    readEntryBytes(*package_, entry_, consumed_, input_.data(), amount);
                !read) {
                return read;
            }
            consumed_ += amount;
            inputOffset_ = 0;
            inputAvailable_ = amount;
        }
        std::size_t sourceSize = inputAvailable_;
        std::size_t destinationSize = output_.size();
        const auto hint = LZ4F_decompress(context_, output_.data(), &destinationSize,
                                          input_.data() + inputOffset_, &sourceSize,
                                          nullptr);
        if (LZ4F_isError(hint)) {
            return lz4Error(entry_, LZ4F_getErrorName(hint));
        }
        inputOffset_ += sourceSize;
        inputAvailable_ -= sourceSize;
        outputOffset_ = 0;
        outputAvailable_ = destinationSize;
        if (hint == 0) {
            frameEnded_ = true;
        }
        if (sourceSize == 0 && destinationSize == 0 && inputAvailable_ > 0) {
            return lz4Error(entry_, "LZ4 decoder made no progress");
        }
    }
    return success();
}

Status Lz4StreamReader::read(std::uint64_t offset, std::uint8_t* destination,
                             std::size_t size)
{
    if (offset != position_) {
        return makeError(ErrorCode::invalidArgument,
                         "LZ4 images can only be read sequentially");
    }
    if (size > decodedSize_ - position_) {
        return lz4Error(entry_, "read past the validated image size");
    }
    std::size_t done = 0;
    while (done < size) {
        if (auto filled = refill(); !filled) {
            return filled;
        }
        const auto count = std::min(outputAvailable_, size - done);
        std::memcpy(destination + done, output_.data() + outputOffset_, count);
        outputOffset_ += count;
        outputAvailable_ -= count;
        done += count;
    }
    position_ += size;
    return success();
}

} // namespace bifrost::firmware
