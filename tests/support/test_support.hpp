// Shared helpers for Bifrost Studio tests: a minimal check harness, a scripted
// transport with fault injection, and PIT / TAR / .tar.md5 builders.
#ifndef BIFROST_TEST_SUPPORT_HPP
#define BIFROST_TEST_SUPPORT_HPP

#include "bifrost/protocol/packet.hpp"
#include "bifrost/transport/transport.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>
#include <unistd.h>
#include <vector>

#if defined(__APPLE__)
#include <CommonCrypto/CommonDigest.h>
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#endif

namespace test {

inline int& failures()
{
    static int count = 0;
    return count;
}

#define CHECK(condition)                                                        \
    do {                                                                        \
        if (!(condition)) {                                                     \
            std::cerr << __FILE__ << ":" << __LINE__ << ": CHECK failed: "      \
                      << #condition << '\n';                                    \
            ++test::failures();                                                 \
        }                                                                       \
    } while (false)

#define CHECK_CONTAINS(text, needle)                                            \
    do {                                                                        \
        const std::string checkText = (text);                                   \
        if (checkText.find(needle) == std::string::npos) {                      \
            std::cerr << __FILE__ << ":" << __LINE__ << ": expected \""         \
                      << (needle) << "\" in: " << checkText << '\n';            \
            ++test::failures();                                                 \
        }                                                                       \
    } while (false)

inline int result()
{
    if (failures() != 0) {
        std::cerr << failures() << " check(s) failed\n";
    }
    return failures() == 0 ? 0 : 1;
}

using Bytes = std::vector<std::uint8_t>;

inline void put32(std::uint8_t* destination, std::uint32_t value)
{
    bifrost::odin::writeLittleEndian32(destination, value);
}

inline Bytes response(std::uint32_t id, std::uint32_t value)
{
    Bytes bytes(8);
    put32(bytes.data(), id);
    put32(bytes.data() + 4, value);
    return bytes;
}

// One scripted read: either data, or an injected failure.
struct Read {
    Bytes data;
    std::optional<bifrost::ErrorCode> failure;
};

inline Read ok(Bytes data) { return {std::move(data), std::nullopt}; }
inline Read fault(bifrost::ErrorCode code) { return {{}, code}; }

// Scripted ClaimedInterface. Reads are consumed in order; writes are recorded.
class ScriptedInterface final : public bifrost::transport::ClaimedInterface {
public:
    explicit ScriptedInterface(std::vector<Read> reads) : reads_(reads.begin(), reads.end()) {}

    const bifrost::transport::OdinInterface& selection() const noexcept override
    {
        return selection_;
    }

    bifrost::Expected<std::size_t> bulkWrite(const std::uint8_t* data, std::size_t size,
                                             unsigned int) override
    {
        if (writeFailureAt && writeCount_ == *writeFailureAt) {
            ++writeCount_;
            return bifrost::makeError(writeFailureCode, "scripted write failure");
        }
        ++writeCount_;
        writes.insert(writes.end(), data, data + size);
        return size;
    }

    bifrost::Expected<std::size_t> bulkRead(std::uint8_t* data, std::size_t size,
                                            unsigned int) override
    {
        if (reads_.empty()) {
            return bifrost::makeError(bifrost::ErrorCode::usbTimeout, "script exhausted");
        }
        auto next = std::move(reads_.front());
        reads_.pop_front();
        ++readsUsed;
        if (next.failure) {
            return bifrost::makeError(*next.failure, "scripted read failure");
        }
        if (next.data.size() > size) {
            return bifrost::makeError(bifrost::ErrorCode::usbOther, "overflow");
        }
        std::copy(next.data.begin(), next.data.end(), data);
        return next.data.size();
    }

    bifrost::Status sendZeroLength(unsigned int) override
    {
        ++zeroLengthWrites;
        if (zeroLengthFailure) {
            return bifrost::makeError(*zeroLengthFailure, "scripted ZLP failure");
        }
        return bifrost::success();
    }

    bifrost::Status release() override { return bifrost::success(); }

    // Control packets written, as (command, request, first argument).
    struct Packet {
        std::uint32_t command, request, argument;
    };
    std::vector<Packet> controlPackets() const
    {
        std::vector<Packet> packets;
        std::size_t offset = 0;
        if (writes.size() >= 4 && std::memcmp(writes.data(), "ODIN", 4) == 0) {
            offset = 4;
        }
        while (offset + bifrost::odin::kControlPacketSize <= writes.size()) {
            const auto* p = writes.data() + offset;
            const auto command = bifrost::odin::readLittleEndian32(p);
            if (command >= 0x64 && command <= 0x67) {
                packets.push_back({command, bifrost::odin::readLittleEndian32(p + 4),
                                   bifrost::odin::readLittleEndian32(p + 8)});
                offset += bifrost::odin::kControlPacketSize;
            } else {
                offset += dataPartSize; // raw data part
            }
        }
        return packets;
    }

    bool sent(std::uint32_t command, std::uint32_t request) const
    {
        const auto packets = controlPackets();
        return std::any_of(packets.begin(), packets.end(), [&](const Packet& p) {
            return p.command == command && p.request == request;
        });
    }

    Bytes writes;
    std::size_t readsUsed{};
    std::size_t zeroLengthWrites{};
    std::optional<bifrost::ErrorCode> zeroLengthFailure;
    std::optional<std::size_t> writeFailureAt;
    bifrost::ErrorCode writeFailureCode{bifrost::ErrorCode::usbDisconnected};
    std::size_t dataPartSize{1048576};

private:
    bifrost::transport::OdinInterface selection_{1, 0, 0x81, 0x01};
    std::deque<Read> reads_;
    std::size_t writeCount_{};
};

struct PitSpec {
    std::uint32_t identifier;
    std::string partition;
    std::string flashFilename;
    std::uint32_t binaryType{0};
    std::uint32_t deviceType{2};
    std::uint32_t blockCount{0x1000};
};

inline Bytes makePit(const std::vector<PitSpec>& entries)
{
    Bytes pit(28 + 132 * entries.size(), 0);
    put32(pit.data(), 0x12349876);
    put32(pit.data() + 4, static_cast<std::uint32_t>(entries.size()));
    for (std::size_t i = 0; i < entries.size(); ++i) {
        auto* e = pit.data() + 28 + 132 * i;
        put32(e, entries[i].binaryType);
        put32(e + 4, entries[i].deviceType);
        put32(e + 8, entries[i].identifier);
        put32(e + 24, entries[i].blockCount);
        std::memcpy(e + 36, entries[i].partition.data(), entries[i].partition.size());
        std::memcpy(e + 68, entries[i].flashFilename.data(), entries[i].flashFilename.size());
    }
    return pit;
}

// TAR builder supporting ustar, GNU long names ('L'), PAX ('x') and GNU
// base-256 size fields.
class TarBuilder {
public:
    enum class SizeEncoding { octal, base256 };

    TarBuilder& file(const std::string& name, const Bytes& data,
                     SizeEncoding encoding = SizeEncoding::octal, char type = '0')
    {
        if (name.size() > 99) {
            header("././@LongLink", name.size() + 1, 'L', SizeEncoding::octal);
            Bytes longName(name.begin(), name.end());
            longName.push_back(0);
            payload(longName);
            header(name.substr(0, 99), data.size(), type, encoding);
        } else {
            header(name, data.size(), type, encoding);
        }
        payload(data);
        return *this;
    }

    TarBuilder& pax(const std::string& key, const std::string& value)
    {
        std::string record = " " + key + "=" + value + "\n";
        std::size_t length = record.size() + 1;
        while (std::to_string(length).size() + record.size() != length) {
            ++length;
        }
        record = std::to_string(length) + record;
        header("PaxHeader", record.size(), 'x', SizeEncoding::octal);
        payload(Bytes(record.begin(), record.end()));
        return *this;
    }

    Bytes finish() const
    {
        Bytes out = bytes_;
        out.resize(out.size() + 1024, 0);
        return out;
    }

private:
    void header(const std::string& name, std::uint64_t size, char type,
                SizeEncoding encoding)
    {
        std::array<std::uint8_t, 512> h{};
        std::memcpy(h.data(), name.data(), std::min<std::size_t>(name.size(), 100));
        std::snprintf(reinterpret_cast<char*>(h.data() + 100), 8, "%07o", 0644);
        std::snprintf(reinterpret_cast<char*>(h.data() + 108), 8, "%07o", 0);
        std::snprintf(reinterpret_cast<char*>(h.data() + 116), 8, "%07o", 0);
        if (encoding == SizeEncoding::octal) {
            std::snprintf(reinterpret_cast<char*>(h.data() + 124), 12, "%011llo",
                          static_cast<unsigned long long>(size));
        } else {
            h[124] = 0x80;
            for (int i = 0; i < 8; ++i) {
                h[135 - i] = static_cast<std::uint8_t>(size >> (8 * i));
            }
        }
        std::snprintf(reinterpret_cast<char*>(h.data() + 136), 12, "%011o", 0);
        std::memset(h.data() + 148, ' ', 8);
        h[156] = static_cast<std::uint8_t>(type);
        std::memcpy(h.data() + 257, "ustar", 6);
        h[263] = '0';
        h[264] = '0';
        unsigned sum = 0;
        for (auto b : h) {
            sum += b;
        }
        std::snprintf(reinterpret_cast<char*>(h.data() + 148), 8, "%06o", sum);
        h[155] = ' ';
        bytes_.insert(bytes_.end(), h.begin(), h.end());
    }

    void payload(const Bytes& data)
    {
        bytes_.insert(bytes_.end(), data.begin(), data.end());
        bytes_.resize(bytes_.size() + (512 - data.size() % 512) % 512, 0);
    }

    Bytes bytes_;
};

inline std::string md5Hex(const Bytes& bytes)
{
#if defined(__APPLE__)
    std::array<std::uint8_t, 16> digest{};
    CC_MD5(bytes.data(), static_cast<CC_LONG>(bytes.size()), digest.data());
    constexpr char digits[] = "0123456789abcdef";
    std::string hex;
    for (auto b : digest) {
        hex.push_back(digits[b >> 4]);
        hex.push_back(digits[b & 15]);
    }
    return hex;
#else
#error "md5Hex test helper is implemented for macOS only"
#endif
}

inline Bytes withMd5Trailer(const Bytes& tar, const std::string& name)
{
    Bytes out = tar;
    const auto trailer = md5Hex(tar) + "  " + name + "\n";
    out.insert(out.end(), trailer.begin(), trailer.end());
    return out;
}

class TempDir {
public:
    TempDir()
    {
        char pattern[] = "/tmp/bifrost-test-XXXXXX";
        path_ = ::mkdtemp(pattern);
    }
    ~TempDir() { std::filesystem::remove_all(path_); }
    std::string write(const std::string& name, const Bytes& bytes) const
    {
        const auto path = (path_ / name).string();
        std::ofstream(path, std::ios::binary)
            .write(reinterpret_cast<const char*>(bytes.data()),
                   static_cast<std::streamsize>(bytes.size()));
        return path;
    }

private:
    std::filesystem::path path_;
};

} // namespace test

#if defined(__APPLE__)
#pragma clang diagnostic pop
#endif

#endif
