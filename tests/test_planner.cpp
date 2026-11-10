// Flash planner: mapping, skip-list, refusals, model auto-detection, CSC vs
// HOME_CSC, firmware-PIT comparison, ordering and confirmation.
#include "bifrost/firmware/package.hpp"
#include "bifrost/pit/pit.hpp"
#include "bifrost/planner/plan.hpp"
#include "support/test_support.hpp"

#include <fstream>
#include <iterator>

using namespace bifrost;
using planner::Slot;
using test::Bytes;
using test::TarBuilder;

namespace {

const std::vector<test::PitSpec> kDeviceLayout{
    {1, "BOOTLOADER", "sboot.bin"},
    {2, "BOOT", "boot.img"},
    {3, "SYSTEM", "system.img"},
    {4, "CACHE", "cache.img"},
    {5, "USERDATA", "userdata.img"},
    {6, "RADIO", "modem.bin", 1},
};

pit::Table devicePit(const std::vector<test::PitSpec>& layout = kDeviceLayout)
{
    const auto bytes = test::makePit(layout);
    return *pit::parse(bytes.data(), bytes.size());
}

planner::SlotPackage load(const test::TempDir& dir, Slot slot, const std::string& name,
                          const TarBuilder& tar)
{
    const auto bytes = tar.finish();
    const auto path = dir.write(name, test::withMd5Trailer(bytes, name));
    auto package = firmware::inspectPackage(path);
    if (!package) {
        std::cerr << "fixture failed: " << package.error().message << '\n';
        std::abort();
    }
    return {slot, std::make_shared<const firmware::Package>(std::move(*package))};
}

bool anyContains(const std::vector<std::string>& lines, const std::string& needle)
{
    return std::any_of(lines.begin(), lines.end(), [&](const std::string& line) {
        return line.find(needle) != std::string::npos;
    });
}

} // namespace

int main()
{
    test::TempDir dir;
    const Bytes data(16, 7);
    const auto pit = devicePit();

    // Real PIT from the SM-N920I (Galaxy Note 5, Exynos 7420, UFS), read on
    // 2026-11-08. Identifier 1 is used twice (CPEFS on one logical unit,
    // BOTA0 on another), and USERDATA's FOTA name contains "\r\n".
    {
        std::ifstream file(std::string(BIFROST_FIXTURES) + "/SM-N920I.pit", std::ios::binary);
        const Bytes bytes((std::istreambuf_iterator<char>(file)), {});
        CHECK(bytes.size() == 8192);
        auto parsed = pit::parse(bytes.data(), bytes.size());
        CHECK(parsed && parsed->entries.size() == 22);
        if (parsed) {
            const auto* recovery = parsed->findByName("RECOVERY");
            CHECK(recovery && recovery->identifier == 6 && recovery->binaryType == 0 &&
                  recovery->flashFilename == "recovery.img");
            const auto* radio = parsed->findByName("RADIO");
            CHECK(radio && radio->binaryType == 0 && radio->flashFilename == "modem.bin");
            CHECK(parsed->countIdentifier(1) == 2 && parsed->countIdentifier(6) == 1);
            CHECK(parsed->findByName("USERDATA")->fotaFilename == "remained");

            // The first-write plan: recovery only.
            const auto ap = load(dir, Slot::ap, "AP_N920IDVS3DRA1_RECOVERY.tar",
                                 TarBuilder().file("recovery.img", data));
            const auto plan = planner::buildPlan(*parsed, {ap});
            CHECK(plan.flashable() && plan.files.size() == 1 &&
                  plan.files[0].partition.partitionName == "RECOVERY");
        }
    }

    // A flashable partition whose identifier is shared is refused.
    {
        const auto shared = test::makePit({{1, "CPEFS", "cpefs.img"}, {1, "BOTA0", "-"},
                                           {6, "RECOVERY", "recovery.img"}});
        auto table = pit::parse(shared.data(), shared.size());
        CHECK(table);
        if (table) {
            const auto ap = load(dir, Slot::ap, "AP_N920IDVS3DRA1_SHARED.tar",
                                 TarBuilder().file("cpefs.img", data));
            const auto plan = planner::buildPlan(*table, {ap});
            CHECK(!plan.flashable());
            CHECK(!plan.refusals.empty() &&
                  plan.refusals[0].find("shared") != std::string::npos);
        }
    }

    // Repeated partition names parse (Snapdragon PITs list XBL once per
    // logical unit); compare() pairs them by occurrence, and an image whose
    // flash filename matches both is refused as ambiguous.
    {
        const auto bytes = test::makePit({{2, "XBL", "xbl.elf"}, {3, "XBL", "xbl.elf"},
                                          {6, "RECOVERY", "recovery.img"}});
        auto parsed = pit::parse(bytes.data(), bytes.size());
        CHECK(parsed);
        if (parsed) {
            const auto same = pit::compare(*parsed, *parsed);
            CHECK(same.layoutDifferences.empty() && same.sizeDifferences.empty());
            const auto fewer = test::makePit({{2, "XBL", "xbl.elf"}, {6, "RECOVERY", "recovery.img"}});
            auto other = pit::parse(fewer.data(), fewer.size());
            CHECK(other);
            if (other) {
                CHECK(!pit::compare(*parsed, *other).layoutDifferences.empty());
                CHECK(!pit::compare(*other, *parsed).layoutDifferences.empty());
            }
            const auto ap = load(dir, Slot::ap, "AP_N986U1UESEHYH1_XBL.tar",
                                 TarBuilder().file("xbl.elf", data));
            const auto plan = planner::buildPlan(*parsed, {ap});
            CHECK(!plan.flashable());
        }
    }

    const auto bl = load(dir, Slot::bl, "BL_N950FXXU8DUH1_CL1.tar.md5",
                         TarBuilder().file("sboot.bin.lz4", data));
    const auto ap = load(dir, Slot::ap, "AP_N950FXXU8DUH1_CL1.tar.md5",
                         TarBuilder()
                             .file("boot.img", data)
                             .file("system.img", data)
                             .file("meta-data/fota.zip", data));
    const auto homeCsc =
        load(dir, Slot::csc, "HOME_CSC_OXM_N950FOXM8DUH1_CL1.tar.md5",
             TarBuilder().file("cache.img", data).file("N950F.pit", test::makePit(kDeviceLayout)));

    // A complete, valid plan: ordered BL then AP then CSC; metadata skipped.
    {
        const auto plan = planner::buildPlan(pit, {homeCsc, ap, bl});
        CHECK(plan.flashable());
        CHECK(plan.files.size() == 4);
        if (plan.files.size() == 4) {
            CHECK(plan.files[0].slot == Slot::bl && plan.files[0].partition.identifier == 1);
            CHECK(plan.files[1].entry.name == "boot.img");
            CHECK(plan.files[3].slot == Slot::csc);
        }
        CHECK(plan.skipped.size() == 2); // meta-data/fota.zip and N950F.pit
        CHECK(plan.detectedModel == "N950F");
        CHECK(plan.refusals.empty());
        CHECK(plan.bootloaderRevision && *plan.bootloaderRevision == 8);
        CHECK(!plan.erasesUserData && !plan.requiresAcknowledgement());

        CHECK(planner::checkConfirmation(plan, {"SM-N950F", false}));
        CHECK(planner::checkConfirmation(plan, {"n950f", false}));
        auto wrong = planner::checkConfirmation(plan, {"SM-N950U", false});
        CHECK(!wrong && wrong.error().code == ErrorCode::modelMismatch);
        CHECK(!planner::checkConfirmation(plan, {"", false}));
    }

    // Full CSC requires acknowledging the data erase.
    {
        const auto csc = load(dir, Slot::csc, "CSC_OXM_N950FOXM8DUH1_CL1.tar.md5",
                              TarBuilder().file("cache.img", data));
        const auto plan = planner::buildPlan(pit, {ap, csc});
        CHECK(plan.flashable() && plan.erasesUserData && plan.requiresAcknowledgement());
        CHECK(!planner::checkConfirmation(plan, {"SM-N950F", false}));
        CHECK(planner::checkConfirmation(plan, {"SM-N950F", true}));
    }

    // Writing USERDATA requires acknowledgement too.
    {
        const auto userdata = load(dir, Slot::userdata, "userdata.tar.md5",
                                   TarBuilder().file("userdata.img", data));
        const auto plan = planner::buildPlan(pit, {userdata});
        CHECK(plan.flashable() && plan.erasesUserData && plan.requiresAcknowledgement());
    }

    // Packages for different models are refused.
    {
        const auto otherAp = load(dir, Slot::ap, "AP_N986BXXU3DVA1_CL1.tar.md5",
                                  TarBuilder().file("boot.img", data));
        const auto plan = planner::buildPlan(pit, {bl, otherAp});
        CHECK(!plan.flashable() && anyContains(plan.refusals, "different models"));
    }

    // A CSC for another model is refused (model read from the CSC build).
    {
        const auto otherCsc = load(dir, Slot::csc, "HOME_CSC_OXM_N986BOXM3DVA1_CL1.tar.md5",
                                   TarBuilder().file("cache.img", data));
        const auto plan = planner::buildPlan(pit, {ap, otherCsc});
        CHECK(!plan.flashable() && anyContains(plan.refusals, "different models"));
    }

    // A package placed in the wrong slot is refused.
    {
        auto misplaced = ap;
        misplaced.slot = Slot::bl;
        const auto plan = planner::buildPlan(pit, {misplaced});
        CHECK(!plan.flashable() && anyContains(plan.refusals, "slot"));
    }

    // Unknown entries, modem partitions, and duplicates are refused.
    {
        const auto unknown = load(dir, Slot::ap, "AP_custom.tar.md5",
                                  TarBuilder().file("mystery.bin", data));
        CHECK(!planner::buildPlan(pit, {unknown}).flashable());

        const auto cp = load(dir, Slot::cp, "CP_N950FXXU8DUH1_CL1.tar.md5",
                             TarBuilder().file("modem.bin", data));
        const auto modem = planner::buildPlan(pit, {cp});
        CHECK(!modem.flashable() && anyContains(modem.refusals, "modem-type"));

        const auto duplicate = load(dir, Slot::csc, "HOME_CSC_OXM_N950FOXM8DUH1_X.tar.md5",
                                    TarBuilder().file("boot.img", data));
        const auto twice = planner::buildPlan(pit, {ap, duplicate});
        CHECK(!twice.flashable() && anyContains(twice.refusals, "targeted by both"));
    }

    // A CSC whose PIT differs from the device is refused (wrong model or
    // re-partition needed).
    {
        auto otherLayout = kDeviceLayout;
        otherLayout[2].identifier = 30;
        const auto foreign = load(dir, Slot::csc, "HOME_CSC_OXM_N950FOXM8DUH1_Y.tar.md5",
                                  TarBuilder()
                                      .file("cache.img", data)
                                      .file("other.pit", test::makePit(otherLayout)));
        const auto plan = planner::buildPlan(pit, {foreign});
        CHECK(!plan.flashable() && anyContains(plan.refusals, "does not match the device"));

        auto resized = kDeviceLayout;
        resized[2].blockCount = 0x2000;
        const auto bigger = load(dir, Slot::csc, "HOME_CSC_OXM_N950FOXM8DUH1_Z.tar.md5",
                                 TarBuilder()
                                     .file("cache.img", data)
                                     .file("new.pit", test::makePit(resized)));
        CHECK(!planner::buildPlan(pit, {bigger}).flashable());
    }

    // samePlan detects a different device PIT.
    {
        const auto a = planner::buildPlan(pit, {ap});
        auto changed = kDeviceLayout;
        changed[1].identifier = 20;
        const auto b = planner::buildPlan(devicePit(changed), {ap});
        CHECK(planner::samePlan(a, a));
        CHECK(!planner::samePlan(a, b));
    }

    // PIT comparison helper.
    {
        auto changed = kDeviceLayout;
        changed.pop_back();
        const auto comparison = pit::compare(pit, devicePit(changed));
        CHECK(!comparison.sameLayout());
        CHECK(pit::compare(pit, pit).sameLayout());
    }

    return test::result();
}
