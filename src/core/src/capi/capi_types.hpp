// Opaque C API types and helpers shared by the C API sources (not installed).
#ifndef BIFROST_CAPI_TYPES_HPP
#define BIFROST_CAPI_TYPES_HPP

#include "bifrost/bifrost.h"
#include "bifrost/common/error.hpp"
#include "bifrost/firmware/naming.hpp"
#include "bifrost/firmware/package.hpp"
#include "bifrost/pit/pit.hpp"
#include "bifrost/planner/plan.hpp"
#include "bifrost/protocol/session.hpp"
#include "bifrost/transport/transport.hpp"

#include <algorithm>
#include <cstring>
#include <new>
#include <optional>
#include <string>
#include <vector>

using namespace bifrost;

struct bf_device_list {
    transport::UsbEnumeration enumeration;
};

struct bf_packages {
    std::vector<planner::SlotPackage> packages;
    std::vector<firmware::FirmwareName> names;
};

struct bf_pit {
    std::vector<std::uint8_t> bytes;
    pit::Table table;
};

// Built from a FlashPlan (offline, or in-session via the plan callback) or
// received from the background agent (summary only).
struct bf_plan {
    bifrost::planner::PlanSummary summary;
    std::optional<bifrost::planner::FlashPlan> plan;
};

namespace bifrost::capi {

inline void clearError(bf_error* error) noexcept
{
    if (error != nullptr) {
        error->code = BF_OK;
        error->data_state = BF_DATA_NONE;
        error->native_code = 0;
        error->message[0] = '\0';
    }
}

inline void setError(bf_error* error, std::int32_t code, const std::string& message,
              std::int32_t dataState = BF_DATA_NONE, std::int32_t native = 0) noexcept
{
    if (error == nullptr) {
        return;
    }
    error->code = code;
    error->data_state = dataState;
    error->native_code = native;
    std::size_t length = std::min(message.size(), sizeof error->message - 1);
    if (length < message.size()) {
        // Do not cut a UTF-8 sequence in half.
        while (length > 0 &&
               (static_cast<unsigned char>(message[length]) & 0xC0) == 0x80) {
            --length;
        }
    }
    std::memcpy(error->message, message.data(), length);
    error->message[length] = '\0';
}

inline void setError(bf_error* error, const Error& e) noexcept
{
    setError(error, static_cast<std::int32_t>(e.code), e.message,
             static_cast<std::int32_t>(e.dataState), e.nativeCode);
}

template <typename Fn, typename R>
inline R guarded(bf_error* error, R failure, Fn&& body) noexcept
{
    try {
        return body();
    } catch (const std::bad_alloc&) {
        setError(error, BF_E_IO, "Out of memory");
    } catch (const std::exception& e) {
        setError(error, BF_E_IO, std::string("Internal error: ") + e.what());
    } catch (...) {
        setError(error, BF_E_IO, "Internal error");
    }
    return failure;
}

inline bf_session_info toC(const odin::SessionInfo& info)
{
    return {info.sessionAck, info.protocolVersion, info.supportsCompressedTransfer,
            info.packetSize, info.partsPerSequence};
}

inline bf_plan makePlan(const planner::FlashPlan& plan)
{
    return bf_plan{planner::summarize(plan), plan};
}

} // namespace bifrost::capi

#endif
