// End-to-end flash orchestration over a scripted transport.
#include "bifrost/flash/flash.hpp"
#include "support/test_support.hpp"

#include <lz4frame.h>

using namespace bifrost;
using planner::Slot;
using test::Bytes;
using test::ok;
using test::response;
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
    Bytes out(LZ4F_compressFrameBound(source.size(), &preferences));
    out.resize(LZ4F_compressFrame(out.data(), out.size(), source.data(), source.size(),
                                  &preferences));
    return out;
}

// Reads up to and including the PIT exchange.
std::vector<test::Read> sessionAndPit(const Bytes& pit)
{
    return {ok({'L', 'O', 'K', 'E'}), ok(response(0x64, 0x00020000)),
            ok(response(0x64, 0)),     ok(response(0x65, static_cast<std::uint32_t>(pit.size()))),
            ok(pit),                   ok(response(0x65, 0))};
}

void appendFile(std::vector<test::Read>& reads)
{
    // Images in these tests are smaller than one 1 MiB part.
    reads.push_back(ok(response(0x66, 0)));
    reads.push_back(ok(response(0x66, 0)));
    reads.push_back(ok(response(0x00, 0)));
    reads.push_back(ok(response(0x66, 0)));
}

} // namespace

int main()
{
    test::TempDir dir;
    const auto pit = test::makePit(kLayout);
    Bytes system(300000);
    for (std::size_t i = 0; i < system.size(); ++i) {
        system[i] = static_cast<std::uint8_t>(i * 7);
    }
    const Bytes boot{'b', 'o', 'o', 't'};
    const auto apTar = TarBuilder()
                           .file("boot.img", boot)
                           .file("system.img.lz4", lz4Compress(system))
                           .file("meta-data/fota.zip", Bytes(8, 1))
                           .finish();
    const auto apName = "AP_N950FXXU8DUH1_CL1.tar.md5";
    const auto apPath = dir.write(apName, test::withMd5Trailer(apTar, apName));

    auto loaded = flash::loadPackages({{Slot::ap, apPath}});
    CHECK(loaded);
    if (!loaded) {
        return test::result();
    }

    // Successful flash: one session, plan built in-session, LZ4 streamed.
    {
        auto reads = sessionAndPit(pit);
        reads.push_back(ok(response(0x64, 0))); // total bytes
        appendFile(reads);
        appendFile(reads);
        reads.push_back(ok(response(0x67, 0)));
        reads.push_back(ok(response(0x67, 0)));
        test::ScriptedInterface usb(reads);

        flash::Request request{*loaded, {"SM-N950F", false}, std::nullopt};
        bool planSeen = false;
        flash::Observer observer;
        observer.onPlan = [&](const planner::FlashPlan& plan) {
            planSeen = plan.flashable() && plan.files.size() == 2 && plan.skipped.size() == 1;
        };
        std::vector<flash::Phase> phases;
        observer.onProgress = [&](const flash::Progress& p) {
            if (phases.empty() || phases.back() != p.phase) {
                phases.push_back(p.phase);
            }
        };
        auto report = flash::flash(usb, request, {}, observer);
        CHECK(report);
        CHECK(planSeen);
        CHECK(report && report->bytesTransferred == boot.size() + system.size());
        CHECK(usb.sent(0x67, 0) && usb.sent(0x67, 1));
        CHECK(std::find(phases.begin(), phases.end(), flash::Phase::checkingImages) !=
              phases.end());
        CHECK(std::find(phases.begin(), phases.end(), flash::Phase::committing) !=
              phases.end());
        CHECK(!phases.empty() && phases.back() == flash::Phase::done);

        // Total bytes announced as the decompressed total.
        const auto packets = usb.controlPackets();
        const auto total = std::find_if(packets.begin(), packets.end(), [](const auto& p) {
            return p.command == 0x64 && p.request == 2;
        });
        CHECK(total != packets.end() && total->argument == boot.size() + system.size());
    }

    // Wrong typed model: refused in-session, session closed, no data sent.
    {
        auto reads = sessionAndPit(pit);
        reads.push_back(ok(response(0x67, 0)));
        test::ScriptedInterface usb(reads);
        auto report = flash::flash(usb, {*loaded, {"SM-G973F", false}, std::nullopt});
        CHECK(!report && report.error().code == ErrorCode::modelMismatch &&
              report.error().dataState == DataState::none);
        CHECK(usb.sent(0x67, 0) && !usb.sent(0x66, 0));
    }

    // Reviewed plan built against a different PIT: refused, session closed.
    {
        auto otherLayout = kLayout;
        otherLayout[0].identifier = 9;
        const auto otherPit = test::makePit(otherLayout);
        auto reviewedTable = pit::parse(otherPit.data(), otherPit.size());
        auto reviewed = planner::buildPlan(*reviewedTable, *loaded);

        auto reads = sessionAndPit(pit);
        reads.push_back(ok(response(0x67, 0)));
        test::ScriptedInterface usb(reads);
        auto report = flash::flash(usb, {*loaded, {"SM-N950F", false}, reviewed});
        CHECK(!report && report.error().code == ErrorCode::planRefused);
        CHECK(usb.sent(0x67, 0) && !usb.sent(0x66, 0));
    }

    // A device whose PIT lacks a partition: refused before any data.
    {
        const auto smallPit = test::makePit({{1, "BOOT", "boot.img"}});
        auto reads = sessionAndPit(smallPit);
        reads.push_back(ok(response(0x67, 0)));
        test::ScriptedInterface usb(reads);
        auto report = flash::flash(usb, {*loaded, {"SM-N950F", false}, std::nullopt});
        CHECK(!report && report.error().code == ErrorCode::planRefused &&
              report.error().dataState == DataState::none);
        CHECK(!usb.sent(0x66, 0));
    }

    // Corrupt LZ4 image: rejected before the device is touched.
    {
        auto corrupt = lz4Compress(system);
        corrupt[corrupt.size() / 2] ^= 0xFF;
        const auto name = "AP_N950FXXU8DUH1_BAD.tar.md5";
        const auto tar = TarBuilder().file("system.img.lz4", corrupt).finish();
        auto bad = flash::loadPackages({{Slot::ap, dir.write(name, test::withMd5Trailer(tar, name))}});
        CHECK(bad);
        test::ScriptedInterface usb({});
        auto report = flash::flash(usb, {*bad, {"SM-N950F", false}, std::nullopt});
        CHECK(!report && report.error().code == ErrorCode::lz4Invalid);
        CHECK(usb.writes.empty());
    }

    // Cancelled before connecting: nothing sent.
    {
        test::ScriptedInterface usb({});
        CancelToken cancel;
        cancel.request(CancelLevel::beforeData);
        auto report = flash::flash(usb, {*loaded, {"SM-N950F", false}, std::nullopt}, {}, {},
                                   &cancel);
        CHECK(!report && report.error().code == ErrorCode::cancelled);
        CHECK(usb.writes.empty());
    }

    // Disconnect during transfer reports partial data.
    {
        auto reads = sessionAndPit(pit);
        reads.push_back(ok(response(0x64, 0)));
        reads.push_back(ok(response(0x66, 0)));
        reads.push_back(ok(response(0x66, 0)));
        reads.push_back(test::fault(ErrorCode::usbDisconnected));
        test::ScriptedInterface usb(reads);
        auto report = flash::flash(usb, {*loaded, {"SM-N950F", false}, std::nullopt});
        CHECK(!report && report.error().code == ErrorCode::usbDisconnected &&
              report.error().dataState == DataState::partial);
        CHECK_CONTAINS(report.error().message, "boot.img");
    }

    // Probe opens and closes a session without PIT or file traffic.
    {
        test::ScriptedInterface usb({ok({'L', 'O', 'K', 'E'}), ok(response(0x64, 0)),
                                     ok(response(0x67, 0))});
        auto info = flash::probe(usb);
        CHECK(info && info->protocolVersion == 0);
        CHECK(!usb.sent(0x65, 1) && !usb.sent(0x66, 0) && !usb.sent(0x64, 1));
    }

    return test::result();
}
