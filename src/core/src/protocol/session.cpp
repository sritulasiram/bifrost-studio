#include "bifrost/protocol/session.hpp"

#include "bifrost/protocol/packet.hpp"
#include "status_text.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <limits>
#include <string>
#include <thread>

namespace bifrost::odin {
namespace {

constexpr std::array<std::uint8_t, 4> kHandshakeRequest{'O', 'D', 'I', 'N'};
constexpr std::array<std::uint8_t, 4> kHandshakeResponse{'L', 'O', 'K', 'E'};

// Session requests (Heimdall SessionSetupPacket.h:35-40).
constexpr std::uint32_t kBeginSession = 0;
constexpr std::uint32_t kTotalBytes = 2;
constexpr std::uint32_t kFilePartSize = 5;
// Begin-session argument: requested maximum protocol version, as odin4 and
// Samsung-Loki/Thor send it (docs/protocol.md, "Odin protocol as implemented").
constexpr std::uint32_t kRequestedProtocol = 0x7FFFFFFF;

// PIT requests (Heimdall PitFilePacket.h:35-38).
constexpr std::uint32_t kPitDump = 1;
constexpr std::uint32_t kPitPart = 2;
constexpr std::uint32_t kPitEnd = 3;
constexpr std::size_t kPitPartSize = 500;
constexpr std::uint32_t kMaximumPitSize = 1024 * 1024;

// End-session requests (Heimdall EndSessionPacket.h:35-36).
constexpr std::uint32_t kEndSession = 0;
constexpr std::uint32_t kReboot = 1;

using detail::kBootloaderFail;
using detail::negative;
using detail::rejected;
constexpr std::size_t kResponseBufferSize = 512;

// Part sizing by protocol version (odin4 odin_protocol.cpp:399-411, Thor
// Odin.cs:62-72).
constexpr std::uint32_t kLegacyPacketSize = 131072;
constexpr std::uint32_t kLegacyPartsPerSequence = 240;
constexpr std::uint32_t kModernPacketSize = 1048576;
constexpr std::uint32_t kModernPartsPerSequence = 30;

const char* stateName(Session::State state)
{
    switch (state) {
    case Session::State::idle: return "idle";
    case Session::State::open: return "open";
    case Session::State::closed: return "closed";
    case Session::State::failed: return "failed";
    }
    return "unknown";
}

} // namespace

Session::Session(transport::ClaimedInterface& interface, Timeouts timeouts)
    : interface_(interface), timeouts_(timeouts)
{
}

Status Session::requireState(State expected, const char* operation) const
{
    if (state_ != expected) {
        return makeError(ErrorCode::illegalState,
                         std::string("Cannot ") + operation + " while the session is " +
                             stateName(state_));
    }
    return success();
}

Error Session::fail(Error error)
{
    state_ = State::failed;
    if (error.dataState == DataState::none && dataSent_) {
        error.dataState = DataState::partial;
    }
    return error;
}

Status Session::writeAll(const std::uint8_t* data, std::size_t size,
                         unsigned int timeoutMs)
{
    std::size_t offset = 0;
    while (offset < size) {
        auto written = interface_.bulkWrite(data + offset, size - offset, timeoutMs);
        if (!written) {
            return written.error();
        }
        if (*written == 0 || *written > size - offset) {
            return makeError(ErrorCode::usbOther,
                             "USB bulk write returned an invalid length");
        }
        offset += *written;
    }
    return success();
}

Expected<std::uint32_t> Session::exchangePacket(const std::uint8_t* packet,
                                                std::uint32_t expectedResponse,
                                                unsigned int timeoutMs)
{
    if (auto written = writeControl(packet, timeoutMs); !written) {
        return written.error();
    }
    std::array<std::uint8_t, kResponseBufferSize> response{};
    auto received = interface_.bulkRead(response.data(), response.size(), timeoutMs);
    if (!received) {
        return received.error();
    }
    if (*received < 8 || *received > response.size()) {
        return makeError(ErrorCode::protocolViolation,
                         "Odin response had an invalid length (" +
                             std::to_string(*received) + " bytes)");
    }
    const auto id = readLittleEndian32(response.data());
    if (id == kBootloaderFail) {
        return makeError(ErrorCode::bootloaderRejected,
                         "Samsung bootloader returned BOOTLOADER_FAIL");
    }
    if (id != expectedResponse) {
        return makeError(ErrorCode::protocolViolation,
                         "Unexpected Odin response ID " + std::to_string(id) +
                             "; expected " + std::to_string(expectedResponse));
    }
    return readLittleEndian32(response.data() + 4);
}

Expected<std::uint32_t> Session::exchange(std::uint32_t command,
                                          std::uint32_t request,
                                          std::initializer_list<std::uint32_t> args,
                                          std::uint32_t expectedResponse,
                                          unsigned int timeoutMs)
{
    const auto packet = encodeControlPacket(command, request, args);
    return exchangePacket(packet.data(), expectedResponse, timeoutMs);
}

// A control packet is 1024 bytes, an exact multiple of the bulk max packet
// size, so nothing on the wire marks the end of the transfer. Heimdall sends
// a zero-length packet after every control packet (BridgeManager.h:157,
// BridgeManager.cpp:699-723). Hardware (SM-N920I, 2026-11-08): without it,
// begin-session and part-size were answered but the next control packet
// (end session) never was.
Status Session::writeControl(const std::uint8_t* packet, unsigned int timeoutMs)
{
    if (auto written = writeAll(packet, kControlPacketSize, timeoutMs); !written) {
        return written;
    }
    return bestEffortZeroLengthOut();
}

Status Session::bestEffortZeroLengthOut(unsigned int timeoutMs)
{
    // Zero-length transfers are best-effort (Heimdall and libmjolnir ignore
    // their failures); only a disconnect is fatal.
    auto sent = interface_.sendZeroLength(timeoutMs != 0 ? timeoutMs : timeouts_.zeroLengthMs);
    if (!sent && sent.error().code == ErrorCode::usbDisconnected) {
        return sent.error();
    }
    return success();
}

Status Session::bestEffortDrain()
{
    auto drained = interface_.drainZeroLength(timeouts_.zeroLengthMs);
    if (!drained && drained.error().code == ErrorCode::usbDisconnected) {
        return drained.error();
    }
    return success();
}

Expected<SessionInfo> Session::open(bool resume)
{
    if (auto state = requireState(State::idle, "open a session"); !state) {
        return state.error();
    }
    if (!resume) {
        if (auto opened = handshake(); !opened) {
            return fail(opened.error());
        }
    }
    return beginSession();
}

Status Session::handshake()
{
    // The bootloader's USB hardware can accept the bytes before its Odin code
    // is listening (SM8250 / SM-N986U1: the write succeeds, LOKE never comes
    // back, 2026-11-09), so a silent timeout is retried in place a few times,
    // as Heimdall does. Any LOKE that arrives counts, including a late reply
    // to an earlier attempt. Only timeouts are retried.
    constexpr int kAttempts = 4;
    constexpr auto kPause = std::chrono::milliseconds(300);
    Error last = makeError(ErrorCode::usbTimeout, "The handshake was not attempted");
    for (int attempt = 0; attempt < kAttempts; ++attempt) {
        if (attempt != 0) {
            std::this_thread::sleep_for(kPause);
        }
        if (auto written = writeAll(kHandshakeRequest.data(), kHandshakeRequest.size(),
                                    timeouts_.handshakeMs);
            !written) {
            if (written.error().code != ErrorCode::usbTimeout) {
                return written;
            }
            last = written.error();
            continue;
        }
        std::array<std::uint8_t, 7> reply{};
        auto received = interface_.bulkRead(reply.data(), reply.size(),
                                            timeouts_.handshakeMs);
        if (!received) {
            if (received.error().code != ErrorCode::usbTimeout) {
                return received.error();
            }
            last = std::move(received).error();
            continue;
        }
        if (*received != kHandshakeResponse.size() ||
            !std::equal(kHandshakeResponse.begin(), kHandshakeResponse.end(),
                        reply.begin())) {
            return makeError(ErrorCode::protocolViolation,
                             "Unexpected handshake reply; expected LOKE");
        }
        return success();
    }
    last.message +=
        ". The bootloader answers the handshake once per entry into Download "
        "Mode: if an earlier session on this phone ended without a reboot (probe, "
        "read-pit, --no-reboot), retry with --resume; otherwise re-enter Download "
        "Mode. The handshake was attempted " + std::to_string(kAttempts) + " times";
    return last;
}

Expected<SessionInfo> Session::beginSession()
{
    auto ack = exchange(kCommandSession, kBeginSession, {kRequestedProtocol},
                        kCommandSession, timeouts_.controlMs);
    if (!ack) {
        return fail(std::move(ack).error());
    }
    state_ = State::open;

    const auto upper = static_cast<std::uint16_t>(*ack >> 16);
    info_.sessionAck = *ack;
    info_.protocolVersion = static_cast<std::uint16_t>(upper & 0x7FFF);
    info_.supportsCompressedTransfer = (upper & 0x8000) != 0;
    const bool modern = info_.protocolVersion > 1;
    info_.packetSize = modern ? kModernPacketSize : kLegacyPacketSize;
    info_.partsPerSequence = modern ? kModernPartsPerSequence : kLegacyPartsPerSequence;

    if (modern) {
        auto negotiated = exchange(kCommandSession, kFilePartSize,
                                   {info_.packetSize}, kCommandSession,
                                   timeouts_.controlMs);
        if (!negotiated || negative(*negotiated)) {
            Error error = negotiated ? rejected("the part size", *negotiated)
                                     : std::move(negotiated).error();
            // Close the session so the device is left in a clean state.
            (void)exchange(kCommandEndSession, kEndSession, {}, kCommandEndSession,
                           timeouts_.controlMs);
            return fail(std::move(error));
        }
    }
    return info_;
}

Expected<std::vector<std::uint8_t>> Session::readPit()
{
    if (auto state = requireState(State::open, "read the PIT"); !state) {
        return state.error();
    }
    auto size = exchange(kCommandPit, kPitDump, {}, kCommandPit, timeouts_.controlMs);
    if (!size) {
        return fail(std::move(size).error());
    }
    if (*size == 0 || *size > kMaximumPitSize) {
        return fail(makeError(ErrorCode::protocolViolation,
                              "Device reported an invalid PIT size: " +
                                  std::to_string(*size)));
    }
    std::vector<std::uint8_t> bytes(*size);
    const auto parts =
        (*size + static_cast<std::uint32_t>(kPitPartSize) - 1) /
        static_cast<std::uint32_t>(kPitPartSize);
    for (std::uint32_t index = 0; index < parts; ++index) {
        const auto request = encodeControlPacket(kCommandPit, kPitPart, {index});
        if (auto written = writeControl(request.data(), timeouts_.controlMs); !written) {
            return fail(written.error());
        }
        const auto offset = static_cast<std::size_t>(index) * kPitPartSize;
        const auto expected = std::min(kPitPartSize, bytes.size() - offset);
        std::array<std::uint8_t, kPitPartSize> part{};
        auto received = interface_.bulkRead(part.data(), part.size(), timeouts_.controlMs);
        if (!received) {
            return fail(received.error());
        }
        if (*received < expected || *received > part.size()) {
            return fail(makeError(ErrorCode::protocolViolation,
                                  "PIT part " + std::to_string(index) +
                                      " had an invalid length"));
        }
        // Copy only the bytes that belong to the PIT (avoids the overflow in
        // Heimdall BridgeManager.cpp:923/953).
        std::copy_n(part.data(), expected, bytes.data() + offset);
    }
    if (auto drained = bestEffortDrain(); !drained) {
        return fail(drained.error());
    }
    if (auto end = exchange(kCommandPit, kPitEnd, {}, kCommandPit, timeouts_.controlMs);
        !end) {
        return fail(std::move(end).error());
    }
    return bytes;
}

Status Session::announceTotalBytes(std::uint64_t totalBytes)
{
    if (auto state = requireState(State::open, "announce the total size"); !state) {
        return state.error();
    }
    // 64-bit little-endian, as odin4 and Thor send it.
    auto value = exchange(kCommandSession, kTotalBytes,
                          {static_cast<std::uint32_t>(totalBytes),
                           static_cast<std::uint32_t>(totalBytes >> 32)},
                          kCommandSession, timeouts_.controlMs);
    if (!value) {
        return fail(std::move(value).error());
    }
    if (negative(*value)) {
        return fail(rejected("the total firmware size", *value));
    }
    return success();
}

Status Session::close(bool reboot)
{
    if (auto state = requireState(State::open, "close the session"); !state) {
        return state.error();
    }
    const auto onFailure = [this](Error error) {
        error.dataState = dataSent_ ? DataState::complete : DataState::none;
        return error;
    };
    auto ended = exchange(kCommandEndSession, kEndSession, {}, kCommandEndSession,
                          timeouts_.controlMs);
    if (!ended || negative(*ended)) {
        state_ = State::failed;
        Error error = ended ? rejected("the session close", *ended) : std::move(ended).error();
        error.message = "Ending the session failed: " + error.message;
        return onFailure(std::move(error));
    }
    state_ = State::closed;
    if (!reboot) {
        return success();
    }

    // The reboot request. Once its packet is fully sent, a phone that drops
    // off the bus instead of answering is restarting, which is what was asked
    // for (SM-N920I, 2026-11-08: every image and the session close were
    // acknowledged, then the reply read failed with LIBUSB_ERROR_OTHER and the
    // phone left Download Mode). A timeout is not treated this way: the phone
    // is still attached and did not confirm.
    const auto request = encodeControlPacket(kCommandEndSession, kReboot);
    if (auto written = writeAll(request.data(), kControlPacketSize, timeouts_.controlMs);
        !written) {
        Error error = written.error();
        error.message = "Sending the reboot request failed: " + error.message;
        return onFailure(std::move(error));
    }
    const auto leftTheBus = [](const Error& error) {
        return error.code == ErrorCode::usbDisconnected || error.code == ErrorCode::usbOther;
    };
    auto zlp = interface_.sendZeroLength(timeouts_.zeroLengthMs);
    if (!zlp && leftTheBus(zlp.error())) {
        rebootUnconfirmed_ = true;
        return success();
    }
    std::array<std::uint8_t, kResponseBufferSize> response{};
    auto received = interface_.bulkRead(response.data(), response.size(), timeouts_.controlMs);
    if (!received) {
        if (leftTheBus(received.error())) {
            rebootUnconfirmed_ = true;
            return success();
        }
        Error error = std::move(received).error();
        error.message = "The phone did not confirm the reboot request: " + error.message;
        return onFailure(std::move(error));
    }
    if (*received < 8 || *received > response.size()) {
        return onFailure(makeError(ErrorCode::protocolViolation,
                                   "The reboot reply had an invalid length (" +
                                       std::to_string(*received) + " bytes)"));
    }
    const auto id = readLittleEndian32(response.data());
    const auto value = readLittleEndian32(response.data() + 4);
    if (id == kBootloaderFail || id != kCommandEndSession || negative(value)) {
        return onFailure(id == kCommandEndSession ? rejected("the reboot request", value)
                                                  : makeError(ErrorCode::protocolViolation,
                                                              "Unexpected reply to the reboot "
                                                              "request (ID " +
                                                                  std::to_string(id) + ")"));
    }
    return success();
}

} // namespace bifrost::odin
