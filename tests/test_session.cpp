// Odin session state machine against a scripted, fault-injecting transport.
#include "bifrost/protocol/session.hpp"
#include "support/test_support.hpp"

using namespace bifrost;
using odin::Session;
using test::Bytes;
using test::fault;
using test::ok;
using test::response;

namespace {

const Bytes kLoke{'L', 'O', 'K', 'E'};

std::vector<test::Read> openScript(std::uint32_t ack = 0x00020000)
{
    std::vector<test::Read> reads{ok(kLoke), ok(response(0x64, ack))};
    if ((ack >> 16 & 0x7FFF) > 1) {
        reads.push_back(ok(response(0x64, 0))); // part-size negotiation
    }
    return reads;
}

void append(std::vector<test::Read>& reads, std::vector<test::Read> more)
{
    reads.insert(reads.end(), more.begin(), more.end());
}

odin::ImageReader patternReader()
{
    return [](std::uint64_t offset, std::uint8_t* destination, std::size_t size) {
        for (std::size_t i = 0; i < size; ++i) {
            destination[i] = static_cast<std::uint8_t>((offset + i) & 0xFF);
        }
        return success();
    };
}

const odin::PartitionTarget kBoot{7, 0, 2};

// Reads for transferring `parts` data parts in one sequence.
std::vector<test::Read> fileScript(std::uint32_t parts, std::uint32_t endStatus = 0)
{
    std::vector<test::Read> reads{ok(response(0x66, 0)), ok(response(0x66, 0))};
    for (std::uint32_t i = 0; i < parts; ++i) {
        reads.push_back(ok(response(0x00, i)));
    }
    reads.push_back(ok(response(0x66, endStatus)));
    return reads;
}

} // namespace

int main()
{
    // Open (modern protocol): handshake, begin with 0x7FFFFFFF, part size.
    {
        test::ScriptedInterface usb(openScript());
        Session session(usb);
        auto info = session.open();
        CHECK(info && info->protocolVersion == 2 && info->packetSize == 1048576 &&
              info->partsPerSequence == 30);
        CHECK(session.state() == Session::State::open);
        const auto packets = usb.controlPackets();
        CHECK(packets.size() == 2 && packets[0].command == 0x64 &&
              packets[0].request == 0 && packets[0].argument == 0x7FFFFFFF);
        CHECK(packets.size() == 2 && packets[1].request == 5 &&
              packets[1].argument == 1048576);
        CHECK(!usb.sent(0x64, 1)); // the disputed request is never sent
    }

    // Legacy protocol: no part-size request, 128 KiB x 240.
    {
        test::ScriptedInterface usb(openScript(0));
        Session session(usb);
        auto info = session.open();
        CHECK(info && info->packetSize == 131072 && info->partsPerSequence == 240);
        CHECK(!usb.sent(0x64, 5));
    }

    // Bad handshake and BOOTLOADER_FAIL.
    {
        test::ScriptedInterface usb({ok({'N', 'O', 'P', 'E'})});
        Session session(usb);
        auto info = session.open();
        CHECK(!info && info.error().code == ErrorCode::protocolViolation);
        CHECK(session.state() == Session::State::failed);

        test::ScriptedInterface failing({ok(kLoke), ok(response(0xFFFFFFFF, 0))});
        Session rejected(failing);
        auto result = rejected.open();
        CHECK(!result && result.error().code == ErrorCode::bootloaderRejected);
    }

    // Illegal transitions send nothing.
    {
        test::ScriptedInterface usb({});
        Session session(usb);
        CHECK(!session.readPit());
        CHECK(!session.close(false));
        CHECK(usb.writes.empty());
    }

    // PIT download: 500-byte parts, only the declared bytes are kept.
    {
        Bytes pit(1200);
        for (std::size_t i = 0; i < pit.size(); ++i) {
            pit[i] = static_cast<std::uint8_t>(i);
        }
        auto reads = openScript();
        append(reads, {ok(response(0x65, 1200)), ok(Bytes(pit.begin(), pit.begin() + 500)),
                       ok(Bytes(pit.begin() + 500, pit.begin() + 1000)),
                       ok(Bytes(pit.begin() + 1000, pit.end())), ok(response(0x65, 0))});
        test::ScriptedInterface usb(reads);
        Session session(usb);
        CHECK(session.open());
        auto bytes = session.readPit();
        CHECK(bytes && *bytes == pit);
        CHECK(session.state() == Session::State::open);
    }

    // File transfer: padded sequence size, real size and EOF in end packet,
    // zero-filled last part, per-part progress and a committing phase.
    {
        auto reads = openScript();
        append(reads, fileScript(2));
        test::ScriptedInterface usb(reads);
        Session session(usb);
        CHECK(session.open());
        const std::uint64_t size = 1048576 + 10;
        std::vector<std::pair<std::uint64_t, bool>> progress;
        auto sent = session.transferFile(
            kBoot, size, patternReader(),
            [&](std::uint64_t done, bool committing) { progress.push_back({done, committing}); },
            nullptr);
        CHECK(sent);
        CHECK(session.dataSent());
        const auto packets = usb.controlPackets();
        // begin, part-size, 0x66/0, 0x66/2, 0x66/3
        CHECK(packets.size() == 5);
        if (packets.size() == 5) {
            CHECK(packets[3].request == 2 && packets[3].argument == 2 * 1048576);
            CHECK(packets[4].request == 3 && packets[4].argument == 0);
        }
        // End packet fields: real size @12, identifier @24, EOF @28.
        const auto endOffset = usb.writes.size() - bifrost::odin::kControlPacketSize;
        const auto* end = usb.writes.data() + endOffset;
        CHECK(bifrost::odin::readLittleEndian32(end + 12) == size);
        CHECK(bifrost::odin::readLittleEndian32(end + 20) == 2);
        CHECK(bifrost::odin::readLittleEndian32(end + 24) == 7);
        CHECK(bifrost::odin::readLittleEndian32(end + 28) == 1);
        // One ZLP after each of the 5 control packets and one before the
        // end-of-sequence packet; none between data parts.
        CHECK(usb.zeroLengthWrites == packets.size() + 1);
        CHECK(progress.size() == 3 && progress.back() == std::make_pair(size, true));
    }

    // Every negative end-of-sequence status is a failure with partial data.
    for (std::int32_t status = -7; status <= -1; ++status) {
        auto reads = openScript();
        append(reads, fileScript(1, static_cast<std::uint32_t>(status)));
        test::ScriptedInterface usb(reads);
        Session session(usb);
        CHECK(session.open());
        auto sent = session.transferFile(kBoot, 10, patternReader(), {}, nullptr);
        CHECK(!sent && sent.error().code == ErrorCode::bootloaderRejected &&
              sent.error().dataState == DataState::partial);
        CHECK(session.state() == Session::State::failed);
    }

    // Wrong part index and a disconnect mid-transfer.
    {
        auto reads = openScript();
        append(reads, {ok(response(0x66, 0)), ok(response(0x66, 0)), ok(response(0x00, 5))});
        test::ScriptedInterface usb(reads);
        Session session(usb);
        CHECK(session.open());
        auto sent = session.transferFile(kBoot, 10, patternReader(), {}, nullptr);
        CHECK(!sent && sent.error().code == ErrorCode::protocolViolation);

        auto unplugged = openScript();
        append(unplugged, {ok(response(0x66, 0)), ok(response(0x66, 0)),
                           fault(ErrorCode::usbDisconnected)});
        test::ScriptedInterface gone(unplugged);
        Session lost(gone);
        CHECK(lost.open());
        auto result = lost.transferFile(kBoot, 10, patternReader(), {}, nullptr);
        CHECK(!result && result.error().code == ErrorCode::usbDisconnected &&
              result.error().dataState == DataState::partial);
    }

    // Zero-length transfers: a timeout is tolerated, a disconnect is not.
    {
        auto reads = openScript();
        append(reads, fileScript(1));
        test::ScriptedInterface usb(reads);
        usb.zeroLengthFailure = ErrorCode::usbTimeout;
        Session session(usb);
        CHECK(session.open());
        CHECK(session.transferFile(kBoot, 10, patternReader(), {}, nullptr));

        test::ScriptedInterface gone(reads);
        Session lost(gone);
        CHECK(lost.open());
        gone.zeroLengthFailure = ErrorCode::usbDisconnected;
        auto result = lost.transferFile(kBoot, 10, patternReader(), {}, nullptr);
        CHECK(!result && result.error().code == ErrorCode::usbDisconnected);
    }

    // Modem targets are refused before anything is sent.
    {
        test::ScriptedInterface usb(openScript());
        Session session(usb);
        CHECK(session.open());
        const auto before = usb.writes.size();
        auto result = session.transferFile({6, 1, 2}, 10, patternReader(), {}, nullptr);
        CHECK(!result && result.error().code == ErrorCode::planRefused);
        CHECK(usb.writes.size() == before && session.state() == Session::State::open);
    }

    // Cancellation: before data it sends nothing; "after sequence" stops at
    // the next sequence boundary with partial data.
    {
        test::ScriptedInterface usb(openScript());
        Session session(usb);
        CHECK(session.open());
        CancelToken cancel;
        cancel.request(CancelLevel::beforeData);
        const auto before = usb.writes.size();
        auto result = session.transferFile(kBoot, 10, patternReader(), {}, &cancel);
        CHECK(!result && result.error().code == ErrorCode::cancelled &&
              result.error().dataState == DataState::none);
        CHECK(usb.writes.size() == before && session.state() == Session::State::open);

        // Two sequences (legacy: 240 x 128 KiB); cancel after the first.
        auto reads = openScript(0);
        std::vector<test::Read> sequence{ok(response(0x66, 0)), ok(response(0x66, 0))};
        append(reads, sequence);
        for (std::uint32_t i = 0; i < 240; ++i) {
            reads.push_back(ok(response(0x00, i)));
        }
        reads.push_back(ok(response(0x66, 0)));
        test::ScriptedInterface legacy(reads);
        legacy.dataPartSize = 131072;
        Session stoppable(legacy);
        CHECK(stoppable.open());
        CancelToken stop;
        auto stopped = stoppable.transferFile(
            kBoot, 240ull * 131072 + 1, patternReader(),
            [&](std::uint64_t, bool committing) {
                if (committing) {
                    stop.request(CancelLevel::afterSequence);
                }
            },
            &stop);
        CHECK(!stopped && stopped.error().code == ErrorCode::cancelled &&
              stopped.error().dataState == DataState::partial);
    }

    // Close and reboot.
    {
        auto reads = openScript();
        append(reads, {ok(response(0x67, 0)), ok(response(0x67, 0))});
        test::ScriptedInterface usb(reads);
        Session session(usb);
        CHECK(session.open());
        CHECK(session.close(true));
        CHECK(session.state() == Session::State::closed);
        CHECK(usb.sent(0x67, 0) && usb.sent(0x67, 1));
    }

    return test::result();
}
