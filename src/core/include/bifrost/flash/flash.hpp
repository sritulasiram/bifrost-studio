#ifndef BIFROST_FLASH_FLASH_HPP
#define BIFROST_FLASH_FLASH_HPP

#include "bifrost/common/cancel.hpp"
#include "bifrost/common/error.hpp"
#include "bifrost/planner/plan.hpp"
#include "bifrost/protocol/session.hpp"
#include "bifrost/transport/transport.hpp"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace bifrost::flash {

enum class Phase {
    verifying,     // hashing packages
    checkingImages,// decoding LZ4 images once to validate them (no output kept)
    connecting,    // handshake and session setup
    readingPit,
    planning,
    transferring,
    committing,    // waiting for the device to finish writing a sequence
    closing,
    done,
};

const char* toString(Phase phase) noexcept;

struct Progress {
    Phase phase{Phase::verifying};
    std::string slot;
    std::string entry;
    std::uint64_t fileDone{};
    std::uint64_t fileTotal{};
    std::uint64_t totalDone{};
    std::uint64_t total{};
};

enum class LogLevel { info, warning, error };

struct Observer {
    std::function<void(const Progress&)> onProgress;
    std::function<void(LogLevel, const std::string&)> onLog;
    // The plan built inside the flash session, before any data is sent.
    std::function<void(const planner::FlashPlan&)> onPlan;
    // The session as soon as it is open, so a later failure can still report it.
    std::function<void(const odin::SessionInfo&)> onSession;
};

struct SlotPath {
    planner::Slot slot;
    std::string path; // a path, or the display file name when `source` is set
    // Optional already-open file (e.g. a descriptor passed over XPC). When
    // set, `path` is not opened.
    std::shared_ptr<firmware::FileSource> source{};
};

// Verifies and parses every selected package (no device access).
Expected<std::vector<planner::SlotPackage>> loadPackages(
    const std::vector<SlotPath>& paths, const Observer& observer = {},
    const CancelToken* cancel = nullptr);

// One read-only session: handshake, begin, end. Sends no PIT or file data.
// `resume`: skip the handshake (see odin::Session::open).
Expected<odin::SessionInfo> probe(transport::ClaimedInterface& interface,
                                  const odin::Timeouts& timeouts = {}, bool resume = false);

struct DevicePit {
    std::vector<std::uint8_t> bytes;
    pit::Table table;
};

// One read-only session that downloads the raw device PIT (not validated),
// e.g. to keep a copy for diagnosis even if it fails validation.
Expected<std::vector<std::uint8_t>> readDevicePitBytes(transport::ClaimedInterface& interface,
                                                       const odin::Timeouts& timeouts = {},
                                                       bool resume = false);

// One read-only session that downloads and parses the device PIT.
Expected<DevicePit> readDevicePit(transport::ClaimedInterface& interface,
                                  const odin::Timeouts& timeouts = {}, bool resume = false);

// A flash is always the first and only session of a Download Mode entry: it
// performs the ODIN/LOKE handshake and never resumes. Every full flash that
// followed a separate read-PIT session in the same entry (the app's old
// Review step) stalled at the same byte on SM-N920I (2026-11-08), while the
// pre-rebuild single-session engine flashed. Refused paths are fail-safe: a
// phone that already handshook times out at the handshake, before any data.
struct Options {
    bool reboot{true};
    odin::Timeouts timeouts;
};

struct Request {
    std::vector<planner::SlotPackage> packages;
    planner::Confirmation confirmation;
    // Optional plan the user reviewed offline (e.g. against a saved PIT). When
    // set, the in-session plan must be identical or the flash is refused.
    std::optional<planner::FlashPlan> reviewed;
};

struct Report {
    odin::SessionInfo session;
    planner::FlashPlan plan;
    std::uint64_t bytesTransferred{};
    bool rebooted{};
};

// Flashes in exactly one Odin session, opened with a fresh handshake:
//  1. checks every package file is unchanged and decodes each LZ4 image once
//     to validate it and learn its exact size (before touching the device);
//  2. opens the session, reads the device PIT and builds the plan there;
//  3. refuses (closing the session cleanly) on any refusal, a mismatch with
//     the reviewed plan, or a confirmation that does not match the plan;
//  4. streams every image in plan order, then closes (and reboots).
// Errors carry DataState so callers can give accurate recovery guidance.
Expected<Report> flash(transport::ClaimedInterface& interface, const Request& request,
                       const Options& options = {}, const Observer& observer = {},
                       const CancelToken* cancel = nullptr);

} // namespace bifrost::flash

#endif
