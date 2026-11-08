// XPC wire format shared by the agent, the USB helper and their clients.
//
// Every request is a dictionary with "op" (string). Every reply carries
// "ok" (bool); a failed reply carries "error" (an encoded bifrost::Error).
// Events (agent -> subscribed clients, helper -> watchers) are dictionaries
// with "event" (string) and no reply.
#ifndef BIFROST_SERVICE_WIRE_HPP
#define BIFROST_SERVICE_WIRE_HPP

#include "bifrost/common/error.hpp"
#include "bifrost/flash/flash.hpp"
#include "bifrost/planner/plan.hpp"
#include "bifrost/protocol/session.hpp"
#include "bifrost/transport/transport.hpp"

#include <xpc/xpc.h>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace bifrost::service {

// Bumped on any incompatible wire change; peers must match exactly.
inline constexpr std::uint64_t kWireVersion = 1;

// Upper bounds the USB helper enforces on every request.
inline constexpr std::size_t kMaximumTransfer = 2 * 1024 * 1024;
inline constexpr unsigned int kMaximumTimeoutMs = 600000;

// Owning reference to an xpc_object_t (retain/release; no ARC in C++).
class Xpc {
public:
    Xpc() = default;
    static Xpc adopt(xpc_object_t object) { return Xpc(object); } // takes +1
    static Xpc retain(xpc_object_t object)
    {
        return Xpc(object != nullptr ? xpc_retain(object) : nullptr);
    }
    Xpc(const Xpc& other) : object_(other.object_ ? xpc_retain(other.object_) : nullptr) {}
    Xpc(Xpc&& other) noexcept : object_(other.object_) { other.object_ = nullptr; }
    Xpc& operator=(Xpc other) noexcept
    {
        std::swap(object_, other.object_);
        return *this;
    }
    ~Xpc()
    {
        if (object_ != nullptr) {
            xpc_release(object_);
        }
    }
    xpc_object_t get() const noexcept { return object_; }
    explicit operator bool() const noexcept { return object_ != nullptr; }

private:
    explicit Xpc(xpc_object_t object) : object_(object) {}
    xpc_object_t object_{};
};

// ---- dictionary helpers ------------------------------------------------------

Xpc makeRequest(const char* op);
std::string getString(xpc_object_t dictionary, const char* key);
std::optional<std::uint64_t> getUint(xpc_object_t dictionary, const char* key);
std::optional<std::int64_t> getInt(xpc_object_t dictionary, const char* key);
bool getBool(xpc_object_t dictionary, const char* key);
std::vector<std::uint8_t> getData(xpc_object_t dictionary, const char* key);
void putData(xpc_object_t dictionary, const char* key, const std::uint8_t* bytes,
             std::size_t size);

// ---- replies and errors ------------------------------------------------------

Xpc encodeError(const Error& error);
std::optional<Error> decodeError(xpc_object_t encoded);

// A reply to `request` with ok = true (nullptr when no reply is possible).
Xpc replyOk(xpc_object_t request);
// A reply to `request` with ok = false and the error.
Xpc replyError(xpc_object_t request, const Error& error);

// Sends `message` and waits for the reply. XPC failures are mapped to
// `lostCode` (connection interrupted / invalid) or `requirementCode` (the
// peer failed its code-signing requirement). A reply with ok = false becomes
// its encoded error.
Expected<Xpc> call(xpc_connection_t connection, xpc_object_t message,
                   const char* peerName, ErrorCode lostCode,
                   ErrorCode requirementCode);

// ---- domain types ------------------------------------------------------------

void putIdentity(xpc_object_t dictionary, const transport::UsbIdentity& identity);
std::optional<transport::UsbIdentity> getIdentity(xpc_object_t dictionary);

Xpc encodeEnumeration(const transport::UsbEnumeration& enumeration);
transport::UsbEnumeration decodeEnumeration(xpc_object_t dictionary);

void putSelection(xpc_object_t dictionary, const transport::OdinInterface& selection);
transport::OdinInterface getSelection(xpc_object_t dictionary);

Xpc encodeSessionInfo(const odin::SessionInfo& info);
odin::SessionInfo decodeSessionInfo(xpc_object_t dictionary);

Xpc encodeProgress(const flash::Progress& progress);
flash::Progress decodeProgress(xpc_object_t dictionary);

using planner::PlanSummary;
using planner::PlannedFileSummary;

Xpc encodePlan(const PlanSummary& plan);
PlanSummary decodePlan(xpc_object_t dictionary);

struct LogLine {
    flash::LogLevel level{flash::LogLevel::info};
    std::string message;
};

struct JobResult {
    std::optional<Error> error; // empty on success
    odin::SessionInfo session;
    std::uint64_t bytesTransferred{};
    bool rebooted{};
    std::vector<std::uint8_t> pit; // read-pit jobs
};

Xpc encodeResult(const JobResult& result);
JobResult decodeResult(xpc_object_t dictionary);

enum class JobKind { flash, probe, readPit };
const char* toString(JobKind kind) noexcept;
std::optional<JobKind> parseJobKind(const std::string& text);

struct JobSnapshot {
    std::uint64_t id{};
    JobKind kind{JobKind::flash};
    bool running{};
    transport::UsbIdentity device;
    flash::Progress progress;
    std::optional<PlanSummary> plan;
    std::vector<LogLine> log;     // most recent lines
    std::optional<JobResult> result; // set once finished
};

Xpc encodeSnapshot(const JobSnapshot& snapshot);
JobSnapshot decodeSnapshot(xpc_object_t dictionary);

} // namespace bifrost::service

#endif
