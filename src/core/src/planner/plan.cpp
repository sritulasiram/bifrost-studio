#include "bifrost/planner/plan.hpp"

#include <algorithm>
#include <cctype>
#include <unordered_map>
#include <unordered_set>

namespace bifrost::planner {
namespace {

constexpr std::uint64_t kMaximumEmbeddedPit = 1024 * 1024;

std::string lower(std::string text)
{
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return text;
}

bool endsWith(const std::string& text, const std::string& suffix)
{
    return text.size() >= suffix.size() &&
           text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
}

bool isRootPit(const std::string& name)
{
    return name.find('/') == std::string::npos && endsWith(lower(name), ".pit");
}

// Entries known not to be partition images. odin4 skips the same two kinds
// (firmware_package.cpp:825-827).
const char* knownNonImage(const std::string& name)
{
    if (lower(name).compare(0, 10, "meta-data/") == 0) {
        return "Samsung package metadata, not a partition image";
    }
    if (isRootPit(name)) {
        return "PIT layout data; used for the device check, not flashed "
               "(re-partition is not supported)";
    }
    return nullptr;
}

bool slotAccepts(Slot slot, firmware::PackageKind kind)
{
    using firmware::PackageKind;
    switch (kind) {
    case PackageKind::unknown: return true;
    case PackageKind::bl: return slot == Slot::bl;
    case PackageKind::ap: return slot == Slot::ap;
    case PackageKind::cp: return slot == Slot::cp;
    case PackageKind::csc:
    case PackageKind::homeCsc: return slot == Slot::csc;
    case PackageKind::userdata: return slot == Slot::userdata;
    }
    return false;
}

bool isUserDataPartition(const pit::Entry& partition)
{
    const auto name = lower(partition.partitionName);
    return name == "userdata" || name == "data";
}

std::string fileName(const std::string& path)
{
    const auto slash = path.find_last_of('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

void checkEmbeddedPit(const pit::Table& devicePit, const SlotPackage& input,
                      FlashPlan& plan)
{
    for (const auto& entry : input.package->entries) {
        if (!isRootPit(entry.name)) {
            continue;
        }
        if (entry.size == 0 || entry.size > kMaximumEmbeddedPit) {
            plan.refusals.push_back("Firmware PIT " + entry.name +
                                    " has an invalid size");
            continue;
        }
        std::vector<std::uint8_t> bytes(static_cast<std::size_t>(entry.size));
        if (auto read = firmware::readEntryBytes(*input.package, entry, 0,
                                                 bytes.data(), bytes.size());
            !read) {
            plan.refusals.push_back("Could not read firmware PIT " + entry.name +
                                    ": " + read.error().message);
            continue;
        }
        auto table = pit::parse(bytes.data(), bytes.size());
        if (!table) {
            plan.refusals.push_back("Firmware PIT " + entry.name + " is invalid: " +
                                    table.error().message);
            continue;
        }
        const auto comparison = pit::compare(devicePit, *table);
        // Without re-partition support, any difference means the firmware
        // expects a layout this device does not have: refuse.
        for (const auto& difference : comparison.layoutDifferences) {
            plan.refusals.push_back(
                "Firmware PIT does not match the device (likely a different "
                "model): " + difference);
        }
        for (const auto& difference : comparison.sizeDifferences) {
            plan.refusals.push_back(
                "Firmware PIT differs from the device and re-partition is not "
                "supported: " + difference);
        }
    }
}

} // namespace

const char* toString(Slot slot) noexcept
{
    switch (slot) {
    case Slot::bl: return "BL";
    case Slot::ap: return "AP";
    case Slot::cp: return "CP";
    case Slot::csc: return "CSC";
    case Slot::userdata: return "USERDATA";
    }
    return "?";
}

std::optional<Slot> parseSlot(const std::string& text)
{
    const auto value = lower(text);
    if (value == "bl") return Slot::bl;
    if (value == "ap") return Slot::ap;
    if (value == "cp") return Slot::cp;
    if (value == "csc") return Slot::csc;
    if (value == "userdata") return Slot::userdata;
    return std::nullopt;
}

bool FlashPlan::requiresAcknowledgement() const noexcept
{
    return std::any_of(warnings.begin(), warnings.end(),
                       [](const Warning& w) { return w.requiresAcknowledgement; });
}

FlashPlan buildPlan(const pit::Table& devicePit,
                    const std::vector<SlotPackage>& input)
{
    FlashPlan plan;
    plan.inputs = input;
    if (input.empty()) {
        plan.refusals.push_back("Select at least one firmware package");
        return plan;
    }

    auto packages = input;
    std::stable_sort(packages.begin(), packages.end(),
                     [](const SlotPackage& a, const SlotPackage& b) {
                         return static_cast<int>(a.slot) < static_cast<int>(b.slot);
                     });

    std::unordered_set<int> seenSlots;
    std::unordered_map<std::uint32_t, std::string> claimedPartitions;
    std::string modelSource;

    for (const auto& item : packages) {
        const auto slotName = std::string(toString(item.slot));
        if (!item.package) {
            plan.refusals.push_back(slotName + ": package is missing");
            continue;
        }
        if (!seenSlots.insert(static_cast<int>(item.slot)).second) {
            plan.refusals.push_back(slotName + " was selected more than once");
            continue;
        }
        const auto name = firmware::parseFirmwareName(item.package->path);
        const auto file = fileName(item.package->path);

        if (!slotAccepts(item.slot, name.kind)) {
            plan.refusals.push_back(file + " looks like a " +
                                    firmware::toString(name.kind) +
                                    " package but was placed in the " + slotName +
                                    " slot");
        }
        if (!name.modelCode.empty()) {
            if (plan.detectedModel.empty()) {
                plan.detectedModel = name.modelCode;
                modelSource = file;
            } else if (plan.detectedModel != name.modelCode) {
                plan.refusals.push_back("Packages are for different models: " +
                                        modelSource + " is " + plan.detectedModel +
                                        ", " + file + " is " + name.modelCode);
            }
        }
        if (item.slot == Slot::bl && name.bootloaderRevision) {
            plan.bootloaderRevision = name.bootloaderRevision;
        }
        if (!item.package->checksumValid) {
            plan.warnings.push_back(
                {file + " is a plain .tar without an MD5 checksum; its integrity "
                        "cannot be verified",
                 false});
        }
        if (name.kind == firmware::PackageKind::csc) {
            plan.erasesUserData = true;
            plan.warnings.push_back(
                {file + " is a full CSC, which is expected to erase user data. "
                        "Use HOME_CSC to keep data.",
                 true});
        }
        if (item.slot == Slot::csc) {
            checkEmbeddedPit(devicePit, item, plan);
        }

        for (const auto& entry : item.package->entries) {
            if (const char* reason = knownNonImage(entry.name)) {
                plan.skipped.push_back({item.slot, entry.name, reason});
                continue;
            }
            std::string lookup = entry.name;
            if (entry.lz4Compressed) {
                lookup.resize(lookup.size() - 4);
            }
            const pit::Entry* match = nullptr;
            bool ambiguous = false;
            for (const auto& partition : devicePit.entries) {
                if (!partition.flashFilename.empty() &&
                    (partition.flashFilename == entry.name ||
                     partition.flashFilename == lookup)) {
                    ambiguous = match != nullptr;
                    match = &partition;
                    if (ambiguous) {
                        break;
                    }
                }
            }
            if (ambiguous) {
                plan.refusals.push_back(slotName + ": " + entry.name +
                                        " matches more than one PIT partition");
                continue;
            }
            if (match == nullptr) {
                plan.refusals.push_back(slotName + ": " + entry.name +
                                        " does not match any partition on this "
                                        "device");
                continue;
            }
            if (entry.size == 0) {
                plan.refusals.push_back(slotName + ": " + entry.name + " is empty");
                continue;
            }
            if (match->binaryType == pit::kBinaryCommunicationProcessor) {
                // docs/protocol.md, "Open protocol questions".
                plan.refusals.push_back(
                    slotName + ": " + entry.name + " targets modem-type partition " +
                    match->partitionName +
                    ", which is not supported until its transfer layout is "
                    "verified");
                continue;
            }
            if (devicePit.countIdentifier(match->identifier) > 1) {
                plan.refusals.push_back(
                    slotName + ": " + entry.name + " targets partition " +
                    match->partitionName + ", whose identifier " +
                    std::to_string(match->identifier) +
                    " is shared with another partition on this device; the flash "
                    "target would be ambiguous");
                continue;
            }
            const auto claimed =
                claimedPartitions.emplace(match->identifier, slotName + ":" + entry.name);
            if (!claimed.second) {
                plan.refusals.push_back("Partition " + match->partitionName +
                                        " is targeted by both " +
                                        claimed.first->second + " and " + slotName +
                                        ":" + entry.name);
                continue;
            }
            if (item.slot == Slot::userdata || isUserDataPartition(*match)) {
                if (!plan.erasesUserData) {
                    plan.warnings.push_back(
                        {"This plan writes the " + match->partitionName +
                             " partition, which replaces all user data",
                         true});
                }
                plan.erasesUserData = true;
            }
            plan.files.push_back({item.slot, item.package, entry, *match});
        }
    }

    if (plan.files.empty() && plan.refusals.empty()) {
        plan.refusals.push_back("No firmware images map to this device");
    }
    if (plan.detectedModel.empty()) {
        plan.warnings.push_back(
            {"The model could not be read from the firmware file names; check it "
             "carefully before confirming",
             false});
    }
    return plan;
}

bool samePlan(const FlashPlan& a, const FlashPlan& b) noexcept
{
    if (a.files.size() != b.files.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.files.size(); ++i) {
        const auto& x = a.files[i];
        const auto& y = b.files[i];
        if (x.slot != y.slot || x.package != y.package ||
            x.entry.name != y.entry.name ||
            x.partition.identifier != y.partition.identifier ||
            x.partition.binaryType != y.partition.binaryType ||
            x.partition.deviceType != y.partition.deviceType) {
            return false;
        }
    }
    return true;
}

Status checkConfirmation(const FlashPlan& plan, const Confirmation& confirmation)
{
    if (!plan.flashable()) {
        return makeError(ErrorCode::planRefused, "The flash plan has refusals");
    }
    const auto typed = firmware::normalizeModel(confirmation.typedModel);
    if (typed.empty()) {
        return makeError(ErrorCode::modelMismatch,
                         "Type the device model shown on the Download Mode screen");
    }
    if (!plan.detectedModel.empty() && typed != plan.detectedModel) {
        return makeError(ErrorCode::modelMismatch,
                         "Typed model SM-" + typed + " does not match the firmware "
                         "model SM-" + plan.detectedModel);
    }
    if (plan.requiresAcknowledgement() && !confirmation.acknowledgeDataErase) {
        return makeError(ErrorCode::planRefused,
                         "This plan erases user data; acknowledge that to continue");
    }
    return success();
}

PlanSummary summarize(const FlashPlan& plan)
{
    PlanSummary summary;
    for (const auto& f : plan.files) {
        summary.files.push_back({f.slot, f.entry.name, f.partition.partitionName,
                                 f.partition.identifier, f.entry.size, f.entry.lz4Compressed});
    }
    summary.skipped = plan.skipped;
    summary.warnings = plan.warnings;
    summary.refusals = plan.refusals;
    summary.detectedModel = plan.detectedModel;
    summary.bootloaderRevision = plan.bootloaderRevision;
    summary.erasesUserData = plan.erasesUserData;
    summary.requiresAcknowledgement = plan.requiresAcknowledgement();
    summary.flashable = plan.flashable();
    return summary;
}

} // namespace bifrost::planner
