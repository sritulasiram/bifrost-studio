// Odin file transfer (command 0x66), rebuilt from scratch on 2026-11-08 after
// every full flash on SM-N920I stalled at the same byte of system.img.
//
// Wire sequence for one image, which is what the pre-rebuild Gungnir engine
// sent on its data path (the owner reports that build flashed):
//
//   0x66/0                       start file          (control packet + ZLP)
//   for each sequence of up to partsPerSequence parts:
//     0x66/2 {paddedSize}        start sequence      (control packet + ZLP)
//     for each part:
//       part bytes               exactly packetSize, zero-filled tail
//       <- {0x00, partIndex}     8-byte part acknowledgement
//     ZLP
//     0x66/3 {0, realSize, binaryType, deviceType, identifier, eof}
//                                end sequence        (control packet + ZLP)
//     <- {0x66, status}          commit acknowledgement (may take a while)
//
// Deliberately absent, because no hardware result shows they help and each
// was present in some failing run:
//   - a ZLP between data parts (Heimdall BridgeManager.cpp:1090-1091 sends
//     one; parts 2..30 of every sequence were acknowledged without it);
//   - an IN "drain" after the commit acknowledgement (odin4 only; it timed out
//     and aborted the IN pipe after every sequence on SM-N920I).
//
// The ZLP after every control packet is kept: it is what made SM-N920I answer
// the third control packet of a session (hardware test T3, 2026-11-08).

#include "bifrost/protocol/session.hpp"

#include "bifrost/protocol/packet.hpp"
#include "status_text.hpp"

#include <algorithm>
#include <array>
#include <string>

namespace bifrost::odin {
namespace {

// File-transfer requests (Heimdall FileTransferPacket.h:35-38).
constexpr std::uint32_t kStartFile = 0;
constexpr std::uint32_t kStartSequence = 2;
constexpr std::uint32_t kEndSequence = 3;

// A data-part acknowledgement is {0x00, part index}.
constexpr std::uint32_t kPartResponse = 0x00;

// Read with a full max-packet buffer so an unexpectedly long reply is
// reported as a protocol error instead of a libusb overflow.
constexpr std::size_t kAckBufferSize = 512;

} // namespace

// Describes where a transfer failed, so a stall can be told apart from a
// rejection and placed exactly (progress only moves on acknowledgements).
struct Session::TransferCursor {
    std::uint64_t fileSize{};
    std::uint64_t sequences{};
    std::uint64_t sequence{}; // zero-based
    std::uint64_t acknowledged{};
    std::string stage;

    Error annotate(Error error) const
    {
        error.message += " [while " + stage + "; sequence " + std::to_string(sequence + 1) +
                         " of " + std::to_string(sequences) + "; " +
                         std::to_string(acknowledged) + " of " + std::to_string(fileSize) +
                         " bytes of this file acknowledged]";
        return error;
    }
};

Status Session::transferFile(const PartitionTarget& target, std::uint64_t size,
                             const ImageReader& reader, const TransferProgress& progress,
                             const CancelToken* cancel)
{
    if (auto state = requireState(State::open, "transfer a file"); !state) {
        return state.error();
    }
    if (size == 0 || !reader) {
        return makeError(ErrorCode::invalidArgument, "Refusing to transfer an empty image");
    }
    if (target.binaryType == pit::kBinaryCommunicationProcessor) {
        // References disagree on the modem end-of-sequence layout
        // (docs/protocol.md, "Open protocol questions"); refuse before sending anything.
        return makeError(ErrorCode::planRefused,
                         "Modem-type (CP binary) partitions are not supported yet");
    }
    if (info_.packetSize == 0 || info_.partsPerSequence == 0) {
        return makeError(ErrorCode::illegalState, "The session has no negotiated part size");
    }
    if (cancel != nullptr && cancel->honoured(dataSent_)) {
        if (dataSent_) {
            return fail(makeError(ErrorCode::cancelled,
                                  "Flash stopped between images at your request"));
        }
        return makeError(ErrorCode::cancelled, "Flash cancelled before any data was sent");
    }

    const std::uint64_t sequenceBytes =
        static_cast<std::uint64_t>(info_.packetSize) * info_.partsPerSequence;
    TransferCursor cursor;
    cursor.fileSize = size;
    cursor.sequences = (size + sequenceBytes - 1) / sequenceBytes;

    cursor.stage = "starting the file";
    auto started = exchange(kCommandFileTransfer, kStartFile, {}, kCommandFileTransfer,
                            timeouts_.controlMs);
    if (!started) {
        return fail(cursor.annotate(std::move(started).error()));
    }
    if (detail::negative(*started)) {
        return fail(detail::rejected("the file transfer", *started));
    }

    std::vector<std::uint8_t> buffer(info_.packetSize);
    for (; cursor.sequence < cursor.sequences; ++cursor.sequence) {
        const auto realSize = static_cast<std::uint32_t>(
            std::min<std::uint64_t>(sequenceBytes, size - cursor.acknowledged));
        const bool last = cursor.sequence + 1 == cursor.sequences;
        if (auto sent = sendSequence(target, realSize, last, reader, progress, buffer, cursor);
            !sent) {
            return sent;
        }
        if (!last && cancel != nullptr && cancel->honoured(true)) {
            return fail(makeError(ErrorCode::cancelled,
                                  "Flash stopped after a completed sequence at your request"));
        }
    }
    return success();
}

Status Session::sendSequence(const PartitionTarget& target, std::uint32_t realSize, bool last,
                             const ImageReader& reader, const TransferProgress& progress,
                             std::vector<std::uint8_t>& buffer, TransferCursor& cursor)
{
    const std::uint32_t packetSize = info_.packetSize;
    const std::uint32_t parts = (realSize + packetSize - 1) / packetSize;
    // The sequence request carries the padded size; the end packet carries
    // the real size (Heimdall BridgeManager.cpp:1064-1066, 1188-1189).
    const std::uint32_t paddedSize = parts * packetSize;

    cursor.stage = "starting the sequence";
    auto begun = exchange(kCommandFileTransfer, kStartSequence, {paddedSize},
                          kCommandFileTransfer, timeouts_.controlMs);
    if (!begun) {
        return fail(cursor.annotate(std::move(begun).error()));
    }
    if (detail::negative(*begun)) {
        return fail(detail::rejected("the transfer sequence", *begun));
    }

    for (std::uint32_t index = 0; index < parts; ++index) {
        if (auto sent = sendPart(index, parts, reader, buffer, cursor); !sent) {
            return sent;
        }
        if (progress) {
            progress(cursor.acknowledged, false);
        }
    }

    cursor.stage = "ending the sequence";
    if (auto zlp = bestEffortZeroLengthOut(); !zlp) {
        return fail(cursor.annotate(zlp.error()));
    }
    if (progress) {
        progress(cursor.acknowledged, true);
    }
    // Phone layout: destination 0, real size, binary type (0 for AP), device
    // type, partition identifier, end-of-file flag.
    const auto end = encodeControlPacket(
        kCommandFileTransfer, kEndSequence,
        {0, realSize, target.binaryType, target.deviceType, target.identifier,
         last ? 1u : 0u});
    cursor.stage = "waiting for the phone to write the sequence";
    auto committed = exchangePacket(end.data(), kCommandFileTransfer, timeouts_.sequenceEndMs);
    if (!committed) {
        return fail(cursor.annotate(std::move(committed).error()));
    }
    if (detail::negative(*committed)) {
        return fail(detail::rejected("the written data", *committed));
    }
    return success();
}

Status Session::sendPart(std::uint32_t index, std::uint32_t parts, const ImageReader& reader,
                         std::vector<std::uint8_t>& buffer, TransferCursor& cursor)
{
    const auto payload = static_cast<std::size_t>(
        std::min<std::uint64_t>(buffer.size(), cursor.fileSize - cursor.acknowledged));
    std::fill(buffer.begin(), buffer.end(), 0);

    cursor.stage = "reading the firmware image";
    if (auto read = reader(cursor.acknowledged, buffer.data(), payload); !read) {
        // Nothing of this part has been sent; fail() still marks the session
        // partial if earlier parts were.
        return fail(read.error());
    }

    cursor.stage = "sending part " + std::to_string(index + 1) + " of " + std::to_string(parts);
    dataSent_ = true;
    if (auto written = writeAll(buffer.data(), buffer.size(), timeouts_.partMs); !written) {
        return fail(cursor.annotate(written.error()));
    }

    cursor.stage = "waiting for the acknowledgement of part " + std::to_string(index + 1) +
                   " of " + std::to_string(parts);
    std::array<std::uint8_t, kAckBufferSize> ack{};
    auto received = interface_.bulkRead(ack.data(), ack.size(), timeouts_.partMs);
    if (!received) {
        return fail(cursor.annotate(std::move(received).error()));
    }
    if (*received < 8 || *received > ack.size()) {
        return fail(cursor.annotate(makeError(
            ErrorCode::protocolViolation,
            "Data-part acknowledgement had " + std::to_string(*received) +
                " bytes; expected 8")));
    }
    const auto id = readLittleEndian32(ack.data());
    if (id == detail::kBootloaderFail) {
        return fail(cursor.annotate(
            makeError(ErrorCode::bootloaderRejected, "Samsung bootloader returned BOOTLOADER_FAIL")));
    }
    if (id != kPartResponse) {
        return fail(cursor.annotate(makeError(
            ErrorCode::protocolViolation, "Unexpected data-part response ID " + std::to_string(id))));
    }
    const auto acknowledgedIndex = readLittleEndian32(ack.data() + 4);
    if (acknowledgedIndex != index) {
        return fail(cursor.annotate(makeError(
            ErrorCode::protocolViolation, "Expected acknowledgement for part " +
                                              std::to_string(index) + " but got " +
                                              std::to_string(acknowledgedIndex))));
    }
    cursor.acknowledged += payload;
    return success();
}

} // namespace bifrost::odin
