#include "bifrost/firmware/file_source.hpp"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <sys/stat.h>
#include <unistd.h>

namespace bifrost::firmware {
namespace {

#if defined(__APPLE__)
std::int64_t modifiedSeconds(const struct stat& info) { return info.st_mtimespec.tv_sec; }
std::int64_t modifiedNanos(const struct stat& info) { return info.st_mtimespec.tv_nsec; }
#else
std::int64_t modifiedSeconds(const struct stat& info) { return info.st_mtim.tv_sec; }
std::int64_t modifiedNanos(const struct stat& info) { return info.st_mtim.tv_nsec; }
#endif

} // namespace

FileSource::FileSource(std::string path, int descriptor, std::uint64_t size,
                       std::uint64_t device, std::uint64_t inode,
                       std::int64_t seconds, std::int64_t nanos, bool checkPath)
    : path_(std::move(path)), descriptor_(descriptor), size_(size),
      device_(device), inode_(inode), modifiedSeconds_(seconds),
      modifiedNanos_(nanos), checkPath_(checkPath)
{
}

FileSource::~FileSource()
{
    if (descriptor_ >= 0) {
        ::close(descriptor_);
    }
}

Expected<std::shared_ptr<FileSource>> FileSource::open(const std::string& path)
{
    const int descriptor = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (descriptor < 0) {
        return makeError(ErrorCode::ioError, "Could not open " + path + ": " +
                                                 std::strerror(errno));
    }
    struct stat info {};
    if (::fstat(descriptor, &info) != 0 || !S_ISREG(info.st_mode) ||
        info.st_size < 0) {
        ::close(descriptor);
        return makeError(ErrorCode::ioError,
                         "Not a readable regular file: " + path);
    }
    return std::shared_ptr<FileSource>(new FileSource(
        path, descriptor, static_cast<std::uint64_t>(info.st_size),
        static_cast<std::uint64_t>(info.st_dev),
        static_cast<std::uint64_t>(info.st_ino), modifiedSeconds(info),
        modifiedNanos(info), true));
}

Expected<std::shared_ptr<FileSource>> FileSource::adopt(int descriptor,
                                                        std::string displayName)
{
    if (descriptor < 0) {
        return makeError(ErrorCode::invalidArgument,
                         "Invalid file descriptor for " + displayName);
    }
    struct stat info {};
    if (::fstat(descriptor, &info) != 0 || !S_ISREG(info.st_mode) ||
        info.st_size < 0) {
        ::close(descriptor);
        return makeError(ErrorCode::ioError,
                         "Not a readable regular file: " + displayName);
    }
    ::fcntl(descriptor, F_SETFD, FD_CLOEXEC);
    return std::shared_ptr<FileSource>(new FileSource(
        std::move(displayName), descriptor, static_cast<std::uint64_t>(info.st_size),
        static_cast<std::uint64_t>(info.st_dev),
        static_cast<std::uint64_t>(info.st_ino), modifiedSeconds(info),
        modifiedNanos(info), false));
}

Status FileSource::readAt(std::uint64_t offset, std::uint8_t* destination,
                          std::size_t size) const
{
    if (offset > size_ || size > size_ - offset ||
        (destination == nullptr && size != 0)) {
        return makeError(ErrorCode::invalidArgument,
                         "Read range is outside " + path_);
    }
    std::size_t done = 0;
    while (done < size) {
        const auto position = offset + done;
        if (position >
            static_cast<std::uint64_t>(std::numeric_limits<off_t>::max())) {
            return makeError(ErrorCode::ioError, "File offset out of range");
        }
        const ssize_t count = ::pread(descriptor_, destination + done,
                                      size - done, static_cast<off_t>(position));
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count <= 0) {
            return makeError(ErrorCode::ioError,
                             "Could not read " + path_ +
                                 (count < 0 ? std::string(": ") +
                                                  std::strerror(errno)
                                            : std::string(": unexpected end")));
        }
        done += static_cast<std::size_t>(count);
    }
    return success();
}

Status FileSource::verifyUnchanged() const
{
    struct stat opened {};
    struct stat current {};
    if (::fstat(descriptor_, &opened) != 0 ||
        (checkPath_ && ::stat(path_.c_str(), &current) != 0)) {
        return makeError(ErrorCode::fileChanged,
                         "Firmware file is no longer accessible: " + path_);
    }
    const auto same = [this](const struct stat& info) {
        return static_cast<std::uint64_t>(info.st_dev) == device_ &&
               static_cast<std::uint64_t>(info.st_ino) == inode_ &&
               static_cast<std::uint64_t>(info.st_size) == size_ &&
               modifiedSeconds(info) == modifiedSeconds_ &&
               modifiedNanos(info) == modifiedNanos_;
    };
    if (!same(opened) || (checkPath_ && !same(current))) {
        return makeError(ErrorCode::fileChanged,
                         "Firmware file changed after it was verified: " +
                             path_);
    }
    return success();
}

} // namespace bifrost::firmware
