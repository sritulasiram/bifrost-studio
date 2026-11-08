#include "bifrost/firmware/package.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <limits>
#include <optional>
#include <string_view>

#if defined(__APPLE__)
#include <CommonCrypto/CommonDigest.h>
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#else
#include <openssl/evp.h>
#endif

namespace bifrost::firmware {
namespace {

constexpr std::size_t kTarBlockSize = 512;
constexpr std::size_t kHashChunkSize = 1024 * 1024;
constexpr std::size_t kTrailerScanSize = 64 * 1024;
constexpr std::size_t kMaximumTarEntries = 100000;
constexpr std::uint64_t kMaximumMetadataSize = 64 * 1024;

using Block = std::array<std::uint8_t, kTarBlockSize>;

class Md5 {
public:
#if defined(__APPLE__)
    Md5() { CC_MD5_Init(&context_); }
    void update(const std::uint8_t* bytes, std::size_t size)
    {
        CC_MD5_Update(&context_, bytes, static_cast<CC_LONG>(size));
    }
    std::array<std::uint8_t, 16> finish()
    {
        std::array<std::uint8_t, 16> digest{};
        CC_MD5_Final(digest.data(), &context_);
        return digest;
    }

private:
    CC_MD5_CTX context_{};
#else
    Md5() : context_(EVP_MD_CTX_new())
    {
        EVP_DigestInit_ex(context_, EVP_md5(), nullptr);
    }
    ~Md5() { EVP_MD_CTX_free(context_); }
    void update(const std::uint8_t* bytes, std::size_t size)
    {
        EVP_DigestUpdate(context_, bytes, size);
    }
    std::array<std::uint8_t, 16> finish()
    {
        std::array<std::uint8_t, 16> digest{};
        unsigned int size = 0;
        EVP_DigestFinal_ex(context_, digest.data(), &size);
        return digest;
    }

private:
    EVP_MD_CTX* context_;
#endif
};

bool isHex(std::uint8_t value)
{
    return std::isxdigit(value) != 0;
}

std::string lowerHex(const std::array<std::uint8_t, 16>& bytes)
{
    constexpr char digits[] = "0123456789abcdef";
    std::string result(32, '0');
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        result[i * 2] = digits[bytes[i] >> 4];
        result[i * 2 + 1] = digits[bytes[i] & 0x0F];
    }
    return result;
}

std::string toLower(std::string text)
{
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return text;
}

bool endsWith(const std::string& text, std::string_view suffix)
{
    return text.size() >= suffix.size() &&
           text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
}

// Locates the md5sum-style trailer "<32 hex>  <name>\n" in the last bytes of
// the file. The digest must start within the final TAR block; a 512-aligned
// candidate is preferred (the trailer normally follows the TAR end blocks).
std::optional<std::size_t> findMd5Trailer(const std::vector<std::uint8_t>& tail,
                                          std::uint64_t tailOffset,
                                          std::uint64_t fileSize)
{
    if (tail.size() < 32) {
        return std::nullopt;
    }
    const auto candidateAt = [&tail](std::size_t position) {
        if (position + 32 > tail.size() ||
            (position > 0 && isHex(tail[position - 1])) ||
            !std::all_of(tail.begin() + static_cast<std::ptrdiff_t>(position),
                         tail.begin() + static_cast<std::ptrdiff_t>(position + 32),
                         isHex)) {
            return false;
        }
        if (position + 32 == tail.size()) {
            return true;
        }
        const auto next = tail[position + 32];
        if (next != ' ' && next != '\t' && next != '\r' && next != '\n') {
            return false;
        }
        return std::all_of(
            tail.begin() + static_cast<std::ptrdiff_t>(position + 32), tail.end(),
            [](std::uint8_t byte) {
                return byte == '\r' || byte == '\n' || byte == '\t' ||
                       (byte >= 0x20 && byte <= 0x7E);
            });
    };
    for (int pass = 0; pass < 2; ++pass) {
        for (std::size_t pos = tail.size() - 32 + 1; pos-- > 0;) {
            const auto absolute = tailOffset + pos;
            if (fileSize - absolute > kTarBlockSize) {
                break;
            }
            if ((pass == 1 || absolute % kTarBlockSize == 0) && candidateAt(pos)) {
                return pos;
            }
        }
    }
    return std::nullopt;
}

bool parseOctal(const std::uint8_t* bytes, std::size_t size,
                std::uint64_t& value)
{
    value = 0;
    std::size_t position = 0;
    while (position < size && (bytes[position] == ' ' || bytes[position] == 0)) {
        ++position;
    }
    bool foundDigit = false;
    for (; position < size; ++position) {
        const auto digit = bytes[position];
        if (digit == 0 || digit == ' ') {
            break;
        }
        if (digit < '0' || digit > '7' ||
            value > (std::numeric_limits<std::uint64_t>::max() - 7) / 8) {
            return false;
        }
        foundDigit = true;
        value = value * 8 + static_cast<std::uint64_t>(digit - '0');
    }
    return foundDigit;
}

// GNU tar stores sizes >= 8 GiB in big-endian base-256 with bit 7 of the
// first byte set. Only non-negative values that fit in 64 bits are accepted.
bool parseSize(const std::uint8_t* field, std::uint64_t& value)
{
    if ((field[0] & 0x80) == 0) {
        return parseOctal(field, 12, value);
    }
    if ((field[0] & 0x7F) != 0 || field[1] != 0 || field[2] != 0 ||
        field[3] != 0) {
        return false;
    }
    value = 0;
    for (std::size_t i = 4; i < 12; ++i) {
        value = (value << 8) | field[i];
    }
    return true;
}

bool verifyHeaderChecksum(const Block& header)
{
    std::uint64_t stored = 0;
    if (!parseOctal(header.data() + 148, 8, stored)) {
        return false;
    }
    std::uint64_t unsignedSum = 0;
    std::int64_t signedSum = 0;
    for (std::size_t i = 0; i < header.size(); ++i) {
        const bool inField = i >= 148 && i < 156;
        unsignedSum += inField ? ' ' : header[i];
        signedSum += inField ? ' ' : static_cast<std::int8_t>(header[i]);
    }
    // POSIX specifies the unsigned sum; some historic tars use signed bytes.
    return stored == unsignedSum ||
           (signedSum >= 0 && stored == static_cast<std::uint64_t>(signedSum));
}

std::string fixedString(const std::uint8_t* bytes, std::size_t size)
{
    const auto* end = std::find(bytes, bytes + size, std::uint8_t{0});
    return std::string(reinterpret_cast<const char*>(bytes),
                       static_cast<std::size_t>(end - bytes));
}

std::string normalizeEntryName(std::string name)
{
    while (name.compare(0, 2, "./") == 0) {
        name.erase(0, 2);
    }
    return name;
}

bool safeArchiveName(const std::string& name)
{
    if (name.empty() || name.front() == '/' ||
        name.find('\\') != std::string::npos ||
        name.find('\0') != std::string::npos) {
        return false;
    }
    std::size_t start = 0;
    while (start <= name.size()) {
        const auto slash = name.find('/', start);
        const auto component = name.substr(
            start, slash == std::string::npos ? std::string::npos : slash - start);
        if (component == "..") {
            return false;
        }
        if (slash == std::string::npos) {
            break;
        }
        start = slash + 1;
    }
    return true;
}

// Parses PAX extended-header records ("<len> <key>=<value>\n"). Only `path`
// and `size` are used; other keys are ignored.
bool parsePax(const std::vector<std::uint8_t>& data,
              std::optional<std::string>& path,
              std::optional<std::uint64_t>& size)
{
    std::size_t position = 0;
    while (position < data.size()) {
        std::size_t length = 0;
        std::size_t cursor = position;
        while (cursor < data.size() && std::isdigit(data[cursor]) != 0) {
            length = length * 10 + static_cast<std::size_t>(data[cursor] - '0');
            if (length > data.size()) {
                return false;
            }
            ++cursor;
        }
        if (cursor >= data.size() || data[cursor] != ' ' || length == 0 ||
            position + length > data.size() || data[position + length - 1] != '\n') {
            return false;
        }
        const std::string record(
            reinterpret_cast<const char*>(data.data() + cursor + 1),
            position + length - 1 - (cursor + 1));
        const auto equals = record.find('=');
        if (equals == std::string::npos) {
            return false;
        }
        const auto key = record.substr(0, equals);
        const auto value = record.substr(equals + 1);
        if (key == "path") {
            path = value;
        } else if (key == "size") {
            std::uint64_t parsed = 0;
            if (value.empty() ||
                !std::all_of(value.begin(), value.end(),
                             [](char c) { return c >= '0' && c <= '9'; })) {
                return false;
            }
            for (const char c : value) {
                if (parsed > (std::numeric_limits<std::uint64_t>::max() - 9) / 10) {
                    return false;
                }
                parsed = parsed * 10 + static_cast<std::uint64_t>(c - '0');
            }
            size = parsed;
        }
        position += length;
    }
    return true;
}

Error tarError(const std::string& message)
{
    return makeError(ErrorCode::packageInvalid, "Invalid firmware TAR: " + message);
}

Expected<std::vector<ArchiveEntry>> parseTar(const FileSource& source,
                                             std::uint64_t archiveSize)
{
    std::vector<ArchiveEntry> entries;
    std::uint64_t offset = 0;
    Block header{};
    std::optional<std::string> pendingName;
    std::optional<std::uint64_t> pendingSize;
    bool sawEnd = false;

    while (offset + kTarBlockSize <= archiveSize) {
        if (auto read = source.readAt(offset, header.data(), header.size()); !read) {
            return read.error();
        }
        const auto headerOffset = offset;
        offset += kTarBlockSize;
        if (std::all_of(header.begin(), header.end(),
                        [](std::uint8_t b) { return b == 0; })) {
            // Stop at the end marker and ignore anything after it, as Odin does.
            sawEnd = true;
            break;
        }
        if (!verifyHeaderChecksum(header)) {
            return tarError("header checksum failed at byte " +
                            std::to_string(headerOffset));
        }
        std::uint64_t size = 0;
        if (!parseSize(header.data() + 124, size)) {
            return tarError("unsupported size field at byte " +
                            std::to_string(headerOffset));
        }
        const auto type = header[156];
        const auto paddedSize =
            size > std::numeric_limits<std::uint64_t>::max() - (kTarBlockSize - 1)
                ? 0
                : ((size + kTarBlockSize - 1) / kTarBlockSize) * kTarBlockSize;
        if (paddedSize < size || paddedSize > archiveSize - offset) {
            return tarError("entry data is truncated at byte " +
                            std::to_string(headerOffset));
        }

        if (type == 'L' || type == 'x') {
            if (size > kMaximumMetadataSize) {
                return tarError("extended header is too large");
            }
            std::vector<std::uint8_t> data(static_cast<std::size_t>(size));
            if (auto read = source.readAt(offset, data.data(), data.size());
                !read) {
                return read.error();
            }
            if (type == 'L') {
                pendingName = fixedString(data.data(), data.size());
            } else if (!parsePax(data, pendingName, pendingSize)) {
                return tarError("malformed PAX extended header");
            }
            offset += paddedSize;
            continue;
        }

        std::string name = fixedString(header.data(), 100);
        const auto prefix = fixedString(header.data() + 345, 155);
        if (!prefix.empty()) {
            name = prefix + "/" + name;
        }
        if (pendingName) {
            name = *pendingName;
        }
        if (pendingSize) {
            size = *pendingSize;
        }
        pendingName.reset();
        pendingSize.reset();
        name = normalizeEntryName(std::move(name));
        if (!safeArchiveName(name)) {
            return tarError("unsafe or empty entry path");
        }
        const auto dataPadded =
            ((size + kTarBlockSize - 1) / kTarBlockSize) * kTarBlockSize;
        if (dataPadded < size || dataPadded > archiveSize - offset) {
            return tarError("entry data is truncated: " + name);
        }

        if (type == '0' || type == 0) {
            entries.push_back(
                {name, offset, size, endsWith(toLower(name), ".lz4")});
            if (entries.size() > kMaximumTarEntries) {
                return tarError("too many entries");
            }
        } else if (type != '5') {
            return makeError(ErrorCode::archiveUnsupported,
                             "TAR entry type '" +
                                 std::string(1, static_cast<char>(type)) +
                                 "' is not supported (links and special files "
                                 "are rejected): " + name);
        }
        offset += dataPadded;
    }

    if (!sawEnd) {
        return tarError("no end-of-archive block");
    }
    if (pendingName || pendingSize) {
        return tarError("extended header is not followed by an entry");
    }
    if (entries.empty()) {
        return tarError("no regular file entries");
    }
    return entries;
}

} // namespace

namespace {

Status checkPackageName(const std::string& path)
{
    const auto lowerPath = toLower(path);
    if (!endsWith(lowerPath, ".tar.md5") && !endsWith(lowerPath, ".tar")) {
        return makeError(ErrorCode::packageInvalid,
                         "Firmware package must be a .tar or .tar.md5 file");
    }
    return success();
}

} // namespace

Expected<Package> inspectPackage(const std::string& path,
                                 const ByteProgress& hashProgress,
                                 const CancelToken* cancel)
{
    if (auto named = checkPackageName(path); !named) {
        return named.error();
    }
    auto opened = FileSource::open(path);
    if (!opened) {
        return std::move(opened).error();
    }
    return inspectPackage(std::move(opened.value()), hashProgress, cancel);
}

Expected<Package> inspectPackage(std::shared_ptr<FileSource> opened,
                                 const ByteProgress& hashProgress,
                                 const CancelToken* cancel)
{
    if (!opened) {
        return makeError(ErrorCode::invalidArgument, "No firmware file");
    }
    const std::string path = opened->path();
    if (auto named = checkPackageName(path); !named) {
        return named.error();
    }
    const bool hasMd5 = endsWith(toLower(path), ".tar.md5");
    Package package;
    package.source = std::move(opened);
    package.path = path;
    const auto& source = *package.source;
    const auto fileSize = source.size();
    if (fileSize < kTarBlockSize) {
        return makeError(ErrorCode::packageInvalid,
                         "Firmware package is too small to be a TAR archive");
    }
    std::uint64_t contentSize = fileSize;

    if (hasMd5) {
        const auto tailSize = static_cast<std::size_t>(
            std::min<std::uint64_t>(fileSize, kTrailerScanSize));
        const auto tailOffset = fileSize - tailSize;
        std::vector<std::uint8_t> tail(tailSize);
        if (auto read = source.readAt(tailOffset, tail.data(), tail.size()); !read) {
            return read.error();
        }
        const auto digestOffset = findMd5Trailer(tail, tailOffset, fileSize);
        if (!digestOffset) {
            return makeError(ErrorCode::packageInvalid,
                             ".tar.md5 has no valid MD5 trailer");
        }
        contentSize = tailOffset + *digestOffset;
        if (contentSize < kTarBlockSize) {
            return makeError(ErrorCode::packageInvalid,
                             ".tar.md5 trailer precedes a complete TAR archive");
        }
        const std::string expected(
            reinterpret_cast<const char*>(tail.data() + *digestOffset), 32);

        Md5 md5;
        std::vector<std::uint8_t> buffer(kHashChunkSize);
        for (std::uint64_t done = 0; done < contentSize;) {
            if (cancel != nullptr && cancel->honoured(false)) {
                return makeError(ErrorCode::cancelled, "Verification cancelled");
            }
            const auto chunk = static_cast<std::size_t>(
                std::min<std::uint64_t>(buffer.size(), contentSize - done));
            if (auto read = source.readAt(done, buffer.data(), chunk); !read) {
                return read.error();
            }
            md5.update(buffer.data(), chunk);
            done += chunk;
            if (hashProgress) {
                hashProgress(done, contentSize);
            }
        }
        const auto computed = lowerHex(md5.finish());
        if (toLower(expected) != computed) {
            return makeError(ErrorCode::checksumMismatch,
                             ".tar.md5 checksum mismatch: " + path);
        }
        package.checksumPresent = true;
        package.checksumValid = true;
        package.checksumHex = computed;
    }

    auto entries = parseTar(source, contentSize);
    if (!entries) {
        return std::move(entries).error();
    }
    package.entries = std::move(entries.value());
    package.archiveSize = contentSize;
    return package;
}

Status readEntryBytes(const Package& package, const ArchiveEntry& entry,
                      std::uint64_t offset, std::uint8_t* destination,
                      std::size_t size)
{
    if (!package.source || offset > entry.size || size > entry.size - offset ||
        entry.dataOffset > std::numeric_limits<std::uint64_t>::max() - offset) {
        return makeError(ErrorCode::invalidArgument,
                         "Requested TAR entry range is invalid");
    }
    return package.source->readAt(entry.dataOffset + offset, destination, size);
}

} // namespace bifrost::firmware

#if defined(__APPLE__)
#pragma clang diagnostic pop
#endif
