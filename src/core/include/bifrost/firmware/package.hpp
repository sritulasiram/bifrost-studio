#ifndef BIFROST_FIRMWARE_PACKAGE_HPP
#define BIFROST_FIRMWARE_PACKAGE_HPP

#include "bifrost/common/cancel.hpp"
#include "bifrost/common/error.hpp"
#include "bifrost/firmware/file_source.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace bifrost::firmware {

struct ArchiveEntry {
    std::string name;
    std::uint64_t dataOffset{};
    std::uint64_t size{};
    bool lz4Compressed{};
};

struct Package {
    std::shared_ptr<FileSource> source;
    std::string path;
    std::uint64_t archiveSize{};  // bytes covered by the TAR (excludes trailer)
    bool checksumPresent{};
    bool checksumValid{};
    std::string checksumHex;
    std::vector<ArchiveEntry> entries;
};

using ByteProgress = std::function<void(std::uint64_t done, std::uint64_t total)>;

// Verifies a .tar.md5 trailer (hashing every byte before the digest) and
// parses the TAR. A plain .tar is parsed without a checksum.
Expected<Package> inspectPackage(const std::string& path,
                                 const ByteProgress& hashProgress = {},
                                 const CancelToken* cancel = nullptr);

// Same, for an already-open source; `source->path()` (a path or display
// name) decides between .tar and .tar.md5.
Expected<Package> inspectPackage(std::shared_ptr<FileSource> source,
                                 const ByteProgress& hashProgress = {},
                                 const CancelToken* cancel = nullptr);

Status readEntryBytes(const Package& package, const ArchiveEntry& entry,
                      std::uint64_t offset, std::uint8_t* destination,
                      std::size_t size);

} // namespace bifrost::firmware

#endif
