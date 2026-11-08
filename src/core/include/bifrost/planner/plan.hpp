#ifndef BIFROST_PLANNER_PLAN_HPP
#define BIFROST_PLANNER_PLAN_HPP

#include "bifrost/common/error.hpp"
#include "bifrost/firmware/naming.hpp"
#include "bifrost/firmware/package.hpp"
#include "bifrost/pit/pit.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace bifrost::planner {

// Odin slots, in the fixed order in which they are flashed.
enum class Slot { bl, ap, cp, csc, userdata };

const char* toString(Slot slot) noexcept;
std::optional<Slot> parseSlot(const std::string& text);

struct SlotPackage {
    Slot slot;
    std::shared_ptr<const firmware::Package> package;
};

struct PlannedFile {
    Slot slot;
    std::shared_ptr<const firmware::Package> package;
    firmware::ArchiveEntry entry;
    pit::Entry partition;
};

struct SkippedEntry {
    Slot slot;
    std::string entry;
    std::string reason;
};

struct Warning {
    std::string message;
    bool requiresAcknowledgement{}; // e.g. the plan erases user data
};

struct FlashPlan {
    std::vector<SlotPackage> inputs;   // packages the plan was built from
    std::vector<PlannedFile> files;    // flash order
    std::vector<SkippedEntry> skipped; // reported, never silent
    std::vector<Warning> warnings;
    std::vector<std::string> refusals; // any refusal blocks flashing
    std::string detectedModel;         // bare model code, e.g. "N950F"
    std::optional<int> bootloaderRevision;
    bool erasesUserData{};

    bool flashable() const noexcept { return refusals.empty() && !files.empty(); }
    bool requiresAcknowledgement() const noexcept;
};

// Builds a plan against a device PIT. Pure: no device access.
FlashPlan buildPlan(const pit::Table& devicePit,
                    const std::vector<SlotPackage>& packages);

// True when two plans flash the same entries to the same partitions in the
// same order (used to check a reviewed plan against the in-session PIT).
bool samePlan(const FlashPlan& a, const FlashPlan& b) noexcept;

struct Confirmation {
    std::string typedModel;      // what the user typed, e.g. "SM-N950F"
    bool acknowledgeDataErase{}; // required when the plan erases user data
};

// Validates the user's confirmation against the plan.
Status checkConfirmation(const FlashPlan& plan, const Confirmation& confirmation);

// What a UI needs to show a plan, without package handles; built from a
// FlashPlan or received from the background agent.
struct PlannedFileSummary {
    Slot slot{Slot::ap};
    std::string entry;
    std::string partition;
    std::uint32_t partitionId{};
    std::uint64_t size{};
    bool lz4{};
};

struct PlanSummary {
    std::vector<PlannedFileSummary> files;
    std::vector<SkippedEntry> skipped;
    std::vector<Warning> warnings;
    std::vector<std::string> refusals;
    std::string detectedModel;
    std::optional<int> bootloaderRevision;
    bool erasesUserData{};
    bool requiresAcknowledgement{};
    bool flashable{};
};

PlanSummary summarize(const FlashPlan& plan);

} // namespace bifrost::planner

#endif
