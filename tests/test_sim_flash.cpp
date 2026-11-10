// Flash orchestration against the stateful bootloader simulator: byte-exact
// partition contents, protocol v0/1 and v2+, multi-sequence files, and every
// injected fault.
#include "bifrost/flash/flash.hpp"
#include "bifrost/protocol/packet.hpp"
#include "support/sim_device.hpp"
#include "support/test_support.hpp"

#include <lz4frame.h>

using namespace bifrost;
using planner::Slot;
using test::Bytes;
using test::TarBuilder;

namespace {

const std::vector<test::PitSpec> kLayout{
    {1, "BOOT", "boot.img"},
    {2, "SYSTEM", "system.img"},
};

Bytes lz4Compress(const Bytes& source)
{
    LZ4F_preferences_t preferences{};
    preferences.frameInfo.contentSize = source.size();
    preferences.frameInfo.contentChecksumFlag = LZ4F_contentChecksumEnabled;
    Bytes out(LZ4F_compressFrameBound(source.size(), &preferences));
    out.resize(LZ4F_compressFrame(out.data(), out.size(), source.data(), source.size(),
                                  &preferences));
    return out;
}

Bytes pattern(std::size_t size, unsigned seed)
{
    Bytes bytes(size);
    std::uint32_t state = seed * 2654435761u + 1;
    for (std::size_t i = 0; i < size; ++i) {
        // Compressible but not trivially so: runs broken by an LCG.
        if (i % 64 == 0) {
            state = state * 1103515245u + 12345u;
        }
        bytes[i] = static_cast<std::uint8_t>((state >> 16) + (i % 64 < 48 ? 0 : i));
    }
    return bytes;
}

struct Firmware {
    Bytes boot;
    Bytes system;
    std::vector<planner::SlotPackage> packages;
};

Firmware makeFirmware(const test::TempDir& dir, const std::string& tag,
                      std::size_t bootSize, std::size_t systemSize)
{
    Firmware firmware;
    firmware.boot = pattern(bootSize, 1);
    firmware.system = pattern(systemSize, 2);
    const auto tar = TarBuilder()
                         .file("boot.img", firmware.boot)
                         .file("system.img.lz4", lz4Compress(firmware.system))
                         .file("meta-data/fota.zip", Bytes(16, 3))
                         .finish();
    const auto name = "AP_N950FXXU8DUH1_" + tag + ".tar.md5";
    const auto path = dir.write(name, test::withMd5Trailer(tar, name));
    auto loaded = flash::loadPackages({{Slot::ap, path}});
    CHECK(loaded);
    if (loaded) {
        firmware.packages = std::move(*loaded);
    }
    return firmware;
}

sim::Config config(std::uint16_t protocolVersion = 2, sim::Faults faults = {})
{
    sim::Config c;
    c.pit = test::makePit(kLayout);
    c.protocolVersion = protocolVersion;
    c.faults = faults;
    return c;
}

flash::Request request(const Firmware& firmware)
{
    return {firmware.packages, {"SM-N950F", false}, std::nullopt};
}

void checkClean(const sim::Device& device)
{
    for (const auto& v : device.violations()) {
        std::cerr << "simulator violation: " << v << '\n';
    }
    CHECK(device.violations().empty());
    CHECK(!device.sent(0x64, 1));
}

void checkWritten(const sim::Device& device, const Firmware& firmware)
{
    const auto* boot = device.written(1);
    const auto* system = device.written(2);
    CHECK(boot != nullptr && *boot == firmware.boot);
    CHECK(system != nullptr && *system == firmware.system);
    CHECK(device.partitions().at(1).complete && device.partitions().at(2).complete);
}

struct FaultCase {
    const char* name;
    sim::Faults faults;
    ErrorCode code;
    DataState state;
};

} // namespace

int main()
{
    test::TempDir dir;
    // 31 MiB + odd bytes: two sequences on both protocol versions (30 MiB each).
    const auto large = makeFirmware(dir, "LARGE", 700001, 31 * 1024 * 1024 + 777);
    const auto small = makeFirmware(dir, "SMALL", 700001, 2 * 1024 * 1024 + 5);
    if (large.packages.empty() || small.packages.empty()) {
        return test::result();
    }

    // Protocol v2+: 1 MiB parts, multi-sequence LZ4 image, byte-exact result.
    {
        sim::Device device(config(2));
        std::uint64_t lastTotal = 0;
        bool monotonic = true;
        flash::Observer observer;
        observer.onProgress = [&](const flash::Progress& p) {
            if (p.phase == flash::Phase::transferring) {
                monotonic = monotonic && p.totalDone >= lastTotal && p.totalDone <= p.total;
                lastTotal = p.totalDone;
            }
        };
        auto report = flash::flash(device, request(large), {}, observer);
        CHECK(report);
        checkClean(device);
        checkWritten(device, large);
        CHECK(monotonic);
        CHECK(device.partitions().at(2).sequences == 2);
        CHECK(device.beginArgument() == 0x7FFFFFFFu);
        CHECK(device.sent(0x64, 5));
        CHECK(device.announcedTotal() == large.boot.size() + large.system.size());
        CHECK(device.sessionEnded() && device.rebooted());
        CHECK(report && report->session.packetSize == 1048576);
        CHECK(report && report->bytesTransferred == large.boot.size() + large.system.size());
    }

    // Protocol v0/1: 128 KiB parts, no part-size request.
    for (std::uint16_t version : {0, 1}) {
        sim::Device device(config(version));
        flash::Options options;
        options.reboot = false;
        auto report = flash::flash(device, request(large), options);
        CHECK(report);
        checkClean(device);
        checkWritten(device, large);
        CHECK(!device.sent(0x64, 5));
        CHECK(device.partitions().at(2).sequences == 2);
        CHECK(device.sessionEnded() && !device.rebooted());
        CHECK(report && report->session.packetSize == 131072);
    }

    // Regression for the SM-N920I probe (2026-11-08): after 0x64/5, a control
    // packet is answered only once a ZLP ends the transfer.
    {
        sim::Device device(config(2));
        const auto write = [&](const Bytes& bytes) {
            return device.bulkWrite(bytes.data(), bytes.size(), 1000);
        };
        const auto read = [&]() {
            std::array<std::uint8_t, 512> buffer{};
            return device.bulkRead(buffer.data(), buffer.size(), 1000);
        };
        const auto packet = [](std::uint32_t command, std::uint32_t request,
                               std::uint32_t argument) {
            auto p = odin::encodeControlPacket(command, request, {argument});
            return Bytes(p.begin(), p.end());
        };
        CHECK(write({'O', 'D', 'I', 'N'}) && read());
        CHECK(write(packet(0x64, 0, 0x7FFFFFFF)) && read()); // answered without a ZLP
        CHECK(write(packet(0x64, 5, 1048576)) && read());    // still the old buffer size
        CHECK(write(packet(0x67, 0, 0)));
        auto unanswered = read();
        CHECK(!unanswered && unanswered.error().code == ErrorCode::usbTimeout);
        CHECK(device.sendZeroLength(100));
        CHECK(read()); // the ZLP completes the transfer
    }

    // T6 (SM-N920I): the handshake happens once per Download Mode entry. A
    // second session needs resume; without it the handshake times out.
    {
        sim::Device device(config(2));
        CHECK(flash::probe(device));
        auto again = flash::probe(device);
        CHECK(!again && again.error().code == ErrorCode::usbTimeout &&
              again.error().dataState == DataState::none);
        if (!again) {
            CHECK_CONTAINS(again.error().message, "--resume");
        }
        // One attempt per in-place handshake retry (session.cpp).
        CHECK(device.repeatedHandshakes() == 4);
        auto resumed = flash::probe(device, {}, true);
        CHECK(resumed && resumed->protocolVersion == 2);
        auto pit = flash::readDevicePit(device, {}, true);
        CHECK(pit && pit->table.entries.size() == 2);
        // A flash never resumes: after earlier sessions in this entry it fails
        // at the handshake, before any file data.
        auto refused = flash::flash(device, request(small));
        CHECK(!refused && refused.error().code == ErrorCode::usbTimeout &&
              refused.error().dataState == DataState::none);
        CHECK(!device.sent(0x66, 0));
    }

    // A flash on a fresh Download Mode entry is one session with a handshake.
    {
        sim::Device device(config(2));
        auto report = flash::flash(device, request(small));
        CHECK(report);
        checkWritten(device, small);
        CHECK(device.repeatedHandshakes() == 0);
    }

    // The phone restarts on the reboot request without replying (SM-N920I):
    // a successful flash, with a warning in the log.
    for (const auto code : {ErrorCode::usbOther, ErrorCode::usbDisconnected}) {
        sim::Faults f;
        f.rebootDropsOff = code;
        sim::Device device(config(2, f));
        std::vector<std::string> warnings;
        flash::Observer observer;
        observer.onLog = [&](flash::LogLevel level, const std::string& message) {
            if (level == flash::LogLevel::warning) {
                warnings.push_back(message);
            }
        };
        auto report = flash::flash(device, request(small), {}, observer);
        CHECK(report && report->rebooted);
        CHECK(device.sessionEnded() && device.rebooted());
        CHECK(warnings.size() == 1);
        checkWritten(device, small);
    }

    // Probe and PIT read through the simulator.
    {
        sim::Device device(config(2));
        auto info = flash::probe(device);
        CHECK(info && info->protocolVersion == 2);
        CHECK(device.sessionEnded() && !device.sent(0x65, 1) && !device.sent(0x66, 0));
        checkClean(device);
    }
    {
        sim::Device device(config(2));
        auto pit = flash::readDevicePit(device);
        CHECK(pit && pit->bytes == test::makePit(kLayout) && pit->table.entries.size() == 2);
        CHECK(device.sessionEnded() && !device.sent(0x66, 0));
        checkClean(device);
    }

    // Best-effort ZLP: a failing ZLP other than a disconnect is ignored.
    {
        sim::Faults faults;
        faults.zlpOutFailure = ErrorCode::usbStall;
        sim::Device device(config(2, faults));
        auto report = flash::flash(device, request(small));
        CHECK(report);
        CHECK(device.zeroLengthOutCount() > 0);
        checkWritten(device, small);
    }

    // Slow commit within the end-of-sequence timeout succeeds.
    {
        sim::Faults faults;
        faults.commitDelayMs = 5000;
        sim::Device device(config(2, faults));
        flash::Options options;
        options.timeouts.partMs = 1000; // must not be used for the commit wait
        options.timeouts.sequenceEndMs = 10000;
        auto report = flash::flash(device, request(small), options);
        CHECK(report);
        checkWritten(device, small);
    }

    // Fault matrix: each must fail with the right classification.
    std::vector<FaultCase> cases;
    {
        sim::Faults f;
        f.wrongAckAtPart = 3;
        cases.push_back({"wrong part index", f, ErrorCode::protocolViolation,
                         DataState::partial});
    }
    {
        sim::Faults f;
        f.shortAckAtPart = 0;
        cases.push_back({"short ack", f, ErrorCode::protocolViolation, DataState::partial});
    }
    {
        sim::Faults f;
        f.stallAtPart = 2;
        cases.push_back({"stall", f, ErrorCode::usbStall, DataState::partial});
    }
    {
        sim::Faults f;
        f.disconnectAfterBytes = 1024 * 1024 + 1;
        cases.push_back({"disconnect", f, ErrorCode::usbDisconnected, DataState::partial});
    }
    {
        sim::Faults f;
        f.zlpOutFailure = ErrorCode::usbDisconnected;
        f.zlpFailureAfterData = true;
        cases.push_back({"disconnect on ZLP", f, ErrorCode::usbDisconnected,
                         DataState::partial});
    }
    {
        sim::Faults f;
        f.zlpOutFailure = ErrorCode::usbDisconnected;
        cases.push_back({"disconnect on first control ZLP", f, ErrorCode::usbDisconnected,
                         DataState::none});
    }
    {
        sim::Faults f;
        f.lateZlpAfterSequence = 0;
        // Known fail-safe behaviour (docs/status.md, "Known issues").
        cases.push_back({"late ZLP", f, ErrorCode::protocolViolation, DataState::partial});
    }
    {
        sim::Faults f;
        f.commitDelayMs = 5000;
        cases.push_back({"slow commit past timeout", f, ErrorCode::usbTimeout,
                         DataState::partial});
    }
    {
        sim::Faults f;
        f.rejectBeginSession = true;
        cases.push_back({"begin rejected", f, ErrorCode::bootloaderRejected, DataState::none});
    }
    {
        sim::Faults f;
        f.negativeStatus = sim::NegativeStatus{sim::Point::totalBytes, 0, -1};
        cases.push_back({"total bytes rejected", f, ErrorCode::bootloaderRejected,
                         DataState::none});
    }
    {
        sim::Faults f;
        f.negativeStatus = sim::NegativeStatus{sim::Point::fileStart, 0, -1};
        cases.push_back({"file start rejected", f, ErrorCode::bootloaderRejected,
                         DataState::none});
    }
    {
        sim::Faults f;
        f.negativeStatus = sim::NegativeStatus{sim::Point::fileStart, 1, -1};
        cases.push_back({"second file rejected", f, ErrorCode::bootloaderRejected,
                         DataState::partial});
    }
    for (std::int32_t status = -2; status >= -7; --status) {
        sim::Faults f;
        f.negativeStatus = sim::NegativeStatus{sim::Point::sequenceEnd, 0, status};
        cases.push_back({"negative end-of-sequence status", f,
                         ErrorCode::bootloaderRejected, DataState::partial});
    }
    {
        sim::Faults f;
        f.negativeStatus = sim::NegativeStatus{sim::Point::endSession, 0, -1};
        cases.push_back({"end session rejected", f, ErrorCode::bootloaderRejected,
                         DataState::complete});
    }
    {
        sim::Faults f;
        f.negativeStatus = sim::NegativeStatus{sim::Point::reboot, 0, -1};
        cases.push_back({"reboot rejected", f, ErrorCode::bootloaderRejected,
                         DataState::complete});
    }

    {
        sim::Faults f;
        f.rebootSilent = true;
        cases.push_back({"reboot never confirmed, phone still attached", f,
                         ErrorCode::usbTimeout, DataState::complete});
    }

    for (const auto& c : cases) {
        sim::Device device(config(2, c.faults));
        flash::Options options;
        options.timeouts.sequenceEndMs = 1000;
        auto report = flash::flash(device, request(small), options);
        const bool matched = !report && report.error().code == c.code &&
                             report.error().dataState == c.state;
        if (!matched) {
            std::cerr << "fault case '" << c.name << "': "
                      << (report ? std::string("succeeded")
                                 : std::string(toString(report.error().code)) + "/" +
                                       toString(report.error().dataState) + ": " +
                                       report.error().message)
                      << '\n';
        }
        CHECK(matched);
        CHECK(!device.sent(0x64, 1));
    }

    // A transport failure names the stage and the position in the file, so a
    // stall at a sequence boundary can be told from one inside a sequence.
    {
        sim::Faults f;
        f.stallAtPart = 2;
        sim::Device device(config(2, f));
        auto report = flash::flash(device, request(small), {});
        CHECK(!report);
        if (!report) {
            const auto& message = report.error().message;
            CHECK(message.find("[while ") != std::string::npos);
            CHECK(message.find("sequence 1 of ") != std::string::npos);
            CHECK(message.find("bytes of this file acknowledged]") != std::string::npos);
        }
    }

    // Negative statuses carry Thor's labels in the message.
    {
        sim::Faults f;
        f.negativeStatus = sim::NegativeStatus{sim::Point::sequenceEnd, 0, -5};
        sim::Device device(config(2, f));
        auto report = flash::flash(device, request(small));
        CHECK(!report);
        if (!report) {
            CHECK_CONTAINS(report.error().message, "authentication");
        }
    }

    // "Stop after this sequence": honoured at the first boundary only.
    {
        sim::Device device(config(2));
        CancelToken cancel;
        flash::Observer observer;
        observer.onProgress = [&](const flash::Progress& p) {
            if (p.phase == flash::Phase::committing && p.entry == "system.img.lz4") {
                cancel.request(CancelLevel::afterSequence);
            }
        };
        auto report = flash::flash(device, request(large), {}, observer, &cancel);
        CHECK(!report && report.error().code == ErrorCode::cancelled &&
              report.error().dataState == DataState::partial);
        CHECK(device.partitions().at(2).sequences == 1 && !device.partitions().at(2).complete);
        CHECK(device.written(2) && device.written(2)->size() == 30u * 1024 * 1024);
    }

    // Cancel before data (during planning): session closed cleanly, nothing sent.
    {
        sim::Device device(config(2));
        CancelToken cancel;
        flash::Observer observer;
        observer.onPlan = [&](const planner::FlashPlan&) {
            cancel.request(CancelLevel::beforeData);
        };
        auto report = flash::flash(device, request(small), {}, observer, &cancel);
        CHECK(!report && report.error().code == ErrorCode::cancelled &&
              report.error().dataState == DataState::none);
        CHECK(device.dataBytesReceived() == 0);
        CHECK(device.sessionEnded() && !device.sessionLeftOpen());
        checkClean(device);
    }

    // A beforeData cancel arriving mid-transfer is ignored: the flash completes.
    {
        sim::Device device(config(2));
        CancelToken cancel;
        flash::Observer observer;
        observer.onProgress = [&](const flash::Progress& p) {
            if (p.phase == flash::Phase::transferring && p.totalDone > 0) {
                cancel.request(CancelLevel::beforeData);
            }
        };
        auto report = flash::flash(device, request(small), {}, observer, &cancel);
        CHECK(report);
        checkWritten(device, small);
    }

    return test::result();
}
