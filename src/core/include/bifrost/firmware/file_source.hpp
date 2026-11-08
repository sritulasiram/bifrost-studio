#ifndef BIFROST_FIRMWARE_FILE_SOURCE_HPP
#define BIFROST_FIRMWARE_FILE_SOURCE_HPP

#include "bifrost/common/error.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace bifrost::firmware {

// One open, read-only file descriptor. Hashing, parsing and streaming all read
// through the same descriptor, and verifyUnchanged() detects a replaced or
// modified file between verification and transfer.
class FileSource {
public:
    static Expected<std::shared_ptr<FileSource>> open(const std::string& path);
    // Takes ownership of an already-open descriptor (e.g. received over XPC
    // from the UI, which opened the file). `displayName` is used in messages
    // and for the .tar/.tar.md5 decision. verifyUnchanged() then checks the
    // descriptor only: there is no path that could be swapped.
    static Expected<std::shared_ptr<FileSource>> adopt(int descriptor,
                                                       std::string displayName);
    ~FileSource();
    FileSource(const FileSource&) = delete;
    FileSource& operator=(const FileSource&) = delete;

    const std::string& path() const noexcept { return path_; }
    std::uint64_t size() const noexcept { return size_; }

    // Reads exactly `size` bytes at `offset`.
    Status readAt(std::uint64_t offset, std::uint8_t* destination,
                  std::size_t size) const;

    // Fails with fileChanged if the path no longer refers to the same,
    // unmodified file that was opened.
    Status verifyUnchanged() const;

private:
    FileSource(std::string path, int descriptor, std::uint64_t size,
               std::uint64_t device, std::uint64_t inode,
               std::int64_t modifiedSeconds, std::int64_t modifiedNanos,
               bool checkPath);

    std::string path_;
    int descriptor_{-1};
    std::uint64_t size_{};
    std::uint64_t device_{};
    std::uint64_t inode_{};
    std::int64_t modifiedSeconds_{};
    std::int64_t modifiedNanos_{};
    bool checkPath_{true};
};

} // namespace bifrost::firmware

#endif
