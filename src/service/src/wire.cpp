#include "bifrost/service/wire.hpp"

#include <algorithm>

namespace bifrost::service {
namespace {

bool isType(xpc_object_t object, xpc_type_t type)
{
    return object != nullptr && xpc_get_type(object) == type;
}

xpc_object_t child(xpc_object_t dictionary, const char* key, xpc_type_t type)
{
    if (!isType(dictionary, XPC_TYPE_DICTIONARY)) {
        return nullptr;
    }
    xpc_object_t value = xpc_dictionary_get_value(dictionary, key);
    return isType(value, type) ? value : nullptr;
}

void putUint(xpc_object_t d, const char* key, std::uint64_t value)
{
    xpc_dictionary_set_uint64(d, key, value);
}

std::uint64_t uintOr(xpc_object_t d, const char* key, std::uint64_t fallback = 0)
{
    return getUint(d, key).value_or(fallback);
}

void putChild(xpc_object_t dictionary, const char* key, const Xpc& value)
{
    xpc_dictionary_set_value(dictionary, key, value.get());
}

Xpc stringArray(const std::vector<std::string>& values)
{
    auto array = Xpc::adopt(xpc_array_create(nullptr, 0));
    for (const auto& value : values) {
        xpc_array_set_string(array.get(), XPC_ARRAY_APPEND, value.c_str());
    }
    return array;
}

std::vector<std::string> getStringArray(xpc_object_t dictionary, const char* key)
{
    std::vector<std::string> values;
    xpc_object_t array = child(dictionary, key, XPC_TYPE_ARRAY);
    if (array == nullptr) {
        return values;
    }
    const auto count = xpc_array_get_count(array);
    for (std::size_t i = 0; i < count; ++i) {
        const char* text = xpc_array_get_string(array, i);
        if (text != nullptr) {
            values.emplace_back(text);
        }
    }
    return values;
}

template <typename Fn>
void forEachDictionary(xpc_object_t dictionary, const char* key, Fn&& fn)
{
    xpc_object_t array = child(dictionary, key, XPC_TYPE_ARRAY);
    if (array == nullptr) {
        return;
    }
    const auto count = xpc_array_get_count(array);
    for (std::size_t i = 0; i < count; ++i) {
        xpc_object_t item = xpc_array_get_value(array, i);
        if (isType(item, XPC_TYPE_DICTIONARY)) {
            fn(item);
        }
    }
}

planner::Slot slotFrom(std::uint64_t value)
{
    return value <= static_cast<std::uint64_t>(planner::Slot::userdata)
               ? static_cast<planner::Slot>(value)
               : planner::Slot::ap;
}

} // namespace

// ---- dictionary helpers ------------------------------------------------------

Xpc makeRequest(const char* op)
{
    auto request = Xpc::adopt(xpc_dictionary_create(nullptr, nullptr, 0));
    xpc_dictionary_set_string(request.get(), "op", op);
    xpc_dictionary_set_uint64(request.get(), "version", kWireVersion);
    return request;
}

std::string getString(xpc_object_t dictionary, const char* key)
{
    xpc_object_t value = child(dictionary, key, XPC_TYPE_STRING);
    return value != nullptr ? std::string(xpc_string_get_string_ptr(value)) : std::string();
}

std::optional<std::uint64_t> getUint(xpc_object_t dictionary, const char* key)
{
    xpc_object_t value = child(dictionary, key, XPC_TYPE_UINT64);
    if (value == nullptr) {
        return std::nullopt;
    }
    return xpc_uint64_get_value(value);
}

std::optional<std::int64_t> getInt(xpc_object_t dictionary, const char* key)
{
    xpc_object_t value = child(dictionary, key, XPC_TYPE_INT64);
    if (value == nullptr) {
        return std::nullopt;
    }
    return xpc_int64_get_value(value);
}

bool getBool(xpc_object_t dictionary, const char* key)
{
    xpc_object_t value = child(dictionary, key, XPC_TYPE_BOOL);
    return value != nullptr && xpc_bool_get_value(value);
}

std::vector<std::uint8_t> getData(xpc_object_t dictionary, const char* key)
{
    xpc_object_t value = child(dictionary, key, XPC_TYPE_DATA);
    if (value == nullptr) {
        return {};
    }
    const auto* bytes = static_cast<const std::uint8_t*>(xpc_data_get_bytes_ptr(value));
    const auto size = xpc_data_get_length(value);
    return bytes != nullptr ? std::vector<std::uint8_t>(bytes, bytes + size)
                            : std::vector<std::uint8_t>();
}

void putData(xpc_object_t dictionary, const char* key, const std::uint8_t* bytes,
             std::size_t size)
{
    xpc_dictionary_set_data(dictionary, key, size == 0 ? static_cast<const void*>("") : static_cast<const void*>(bytes),
                            size);
}

// ---- replies and errors ------------------------------------------------------

Xpc encodeError(const Error& error)
{
    auto d = Xpc::adopt(xpc_dictionary_create(nullptr, nullptr, 0));
    putUint(d.get(), "code", static_cast<std::uint64_t>(error.code));
    putUint(d.get(), "state", static_cast<std::uint64_t>(error.dataState));
    xpc_dictionary_set_int64(d.get(), "native", error.nativeCode);
    xpc_dictionary_set_string(d.get(), "message", error.message.c_str());
    return d;
}

std::optional<Error> decodeError(xpc_object_t encoded)
{
    if (!isType(encoded, XPC_TYPE_DICTIONARY)) {
        return std::nullopt;
    }
    Error error;
    const auto code = uintOr(encoded, "code", static_cast<std::uint64_t>(ErrorCode::ioError));
    error.code = code >= 1 && code <= static_cast<std::uint64_t>(ErrorCode::serviceUnavailable)
                     ? static_cast<ErrorCode>(code)
                     : ErrorCode::ioError;
    const auto state = uintOr(encoded, "state");
    error.dataState = state <= static_cast<std::uint64_t>(DataState::complete)
                          ? static_cast<DataState>(state)
                          : DataState::partial; // unknown: assume the worst
    error.nativeCode = static_cast<int>(getInt(encoded, "native").value_or(0));
    error.message = getString(encoded, "message");
    return error;
}

Xpc replyOk(xpc_object_t request)
{
    auto reply = Xpc::adopt(xpc_dictionary_create_reply(request));
    if (reply) {
        xpc_dictionary_set_bool(reply.get(), "ok", true);
    }
    return reply;
}

Xpc replyError(xpc_object_t request, const Error& error)
{
    auto reply = Xpc::adopt(xpc_dictionary_create_reply(request));
    if (reply) {
        xpc_dictionary_set_bool(reply.get(), "ok", false);
        putChild(reply.get(), "error", encodeError(error));
    }
    return reply;
}

Expected<Xpc> call(xpc_connection_t connection, xpc_object_t message,
                   const char* peerName, ErrorCode lostCode, ErrorCode requirementCode)
{
    auto reply = Xpc::adopt(xpc_connection_send_message_with_reply_sync(connection, message));
    if (xpc_get_type(reply.get()) == XPC_TYPE_ERROR) {
        if (reply.get() == XPC_ERROR_PEER_CODE_SIGNING_REQUIREMENT) {
            return makeError(requirementCode,
                             std::string("The ") + peerName +
                                 " failed its code-signing check; reinstall Bifrost Studio");
        }
        if (reply.get() == XPC_ERROR_CONNECTION_INTERRUPTED) {
            return makeError(lostCode, std::string("The connection to the ") + peerName +
                                           " was interrupted");
        }
        return makeError(lostCode, std::string("The ") + peerName +
                                       " is not running or not installed");
    }
    if (!isType(reply.get(), XPC_TYPE_DICTIONARY)) {
        return makeError(ErrorCode::protocolViolation,
                         std::string("Malformed reply from the ") + peerName);
    }
    if (!getBool(reply.get(), "ok")) {
        auto error = decodeError(child(reply.get(), "error", XPC_TYPE_DICTIONARY));
        return error ? *error
                     : makeError(ErrorCode::protocolViolation,
                                 std::string("The ") + peerName + " returned no error detail");
    }
    return reply;
}

// ---- domain types ------------------------------------------------------------

void putIdentity(xpc_object_t d, const transport::UsbIdentity& identity)
{
    putUint(d, "vid", identity.vendorId);
    putUint(d, "pid", identity.productId);
    putUint(d, "bus", identity.busNumber);
    putUint(d, "address", identity.deviceAddress);
}

std::optional<transport::UsbIdentity> getIdentity(xpc_object_t d)
{
    const auto vid = getUint(d, "vid");
    const auto pid = getUint(d, "pid");
    const auto bus = getUint(d, "bus");
    const auto address = getUint(d, "address");
    if (!vid || !pid || !bus || !address || *vid > 0xFFFF || *pid > 0xFFFF || *bus > 0xFF ||
        *address > 0xFF) {
        return std::nullopt;
    }
    return transport::UsbIdentity{static_cast<std::uint16_t>(*vid),
                                  static_cast<std::uint16_t>(*pid),
                                  static_cast<std::uint8_t>(*bus),
                                  static_cast<std::uint8_t>(*address)};
}

void putSelection(xpc_object_t d, const transport::OdinInterface& s)
{
    putUint(d, "interface", s.interfaceNumber);
    putUint(d, "alternate", s.alternateSetting);
    putUint(d, "in", s.endpointIn);
    putUint(d, "out", s.endpointOut);
}

transport::OdinInterface getSelection(xpc_object_t d)
{
    return {static_cast<std::uint8_t>(uintOr(d, "interface")),
            static_cast<std::uint8_t>(uintOr(d, "alternate")),
            static_cast<std::uint8_t>(uintOr(d, "in")),
            static_cast<std::uint8_t>(uintOr(d, "out"))};
}

Xpc encodeEnumeration(const transport::UsbEnumeration& enumeration)
{
    auto d = Xpc::adopt(xpc_dictionary_create(nullptr, nullptr, 0));
    auto devices = Xpc::adopt(xpc_array_create(nullptr, 0));
    for (const auto& device : enumeration.devices) {
        auto item = Xpc::adopt(xpc_dictionary_create(nullptr, nullptr, 0));
        putIdentity(item.get(), device.identity);
        xpc_dictionary_set_bool(item.get(), "odin", device.odinInterface.has_value());
        if (device.odinInterface) {
            putSelection(item.get(), *device.odinInterface);
        }
        xpc_array_append_value(devices.get(), item.get());
    }
    putChild(d.get(), "devices", devices);
    putChild(d.get(), "warnings", stringArray(enumeration.warnings));
    return d;
}

transport::UsbEnumeration decodeEnumeration(xpc_object_t d)
{
    transport::UsbEnumeration enumeration;
    forEachDictionary(d, "devices", [&](xpc_object_t item) {
        auto identity = getIdentity(item);
        if (!identity) {
            return;
        }
        transport::UsbDeviceInfo info;
        info.identity = *identity;
        if (getBool(item, "odin")) {
            info.odinInterface = getSelection(item);
        }
        enumeration.devices.push_back(std::move(info));
    });
    enumeration.warnings = getStringArray(d, "warnings");
    return enumeration;
}

Xpc encodeSessionInfo(const odin::SessionInfo& info)
{
    auto d = Xpc::adopt(xpc_dictionary_create(nullptr, nullptr, 0));
    putUint(d.get(), "ack", info.sessionAck);
    putUint(d.get(), "version", info.protocolVersion);
    xpc_dictionary_set_bool(d.get(), "compressed", info.supportsCompressedTransfer);
    putUint(d.get(), "packetSize", info.packetSize);
    putUint(d.get(), "partsPerSequence", info.partsPerSequence);
    return d;
}

odin::SessionInfo decodeSessionInfo(xpc_object_t d)
{
    odin::SessionInfo info;
    info.sessionAck = static_cast<std::uint32_t>(uintOr(d, "ack"));
    info.protocolVersion = static_cast<std::uint16_t>(uintOr(d, "version"));
    info.supportsCompressedTransfer = getBool(d, "compressed");
    info.packetSize = static_cast<std::uint32_t>(uintOr(d, "packetSize"));
    info.partsPerSequence = static_cast<std::uint32_t>(uintOr(d, "partsPerSequence"));
    return info;
}

Xpc encodeProgress(const flash::Progress& p)
{
    auto d = Xpc::adopt(xpc_dictionary_create(nullptr, nullptr, 0));
    putUint(d.get(), "phase", static_cast<std::uint64_t>(p.phase));
    xpc_dictionary_set_string(d.get(), "slot", p.slot.c_str());
    xpc_dictionary_set_string(d.get(), "entry", p.entry.c_str());
    putUint(d.get(), "fileDone", p.fileDone);
    putUint(d.get(), "fileTotal", p.fileTotal);
    putUint(d.get(), "totalDone", p.totalDone);
    putUint(d.get(), "total", p.total);
    return d;
}

flash::Progress decodeProgress(xpc_object_t d)
{
    flash::Progress p;
    const auto phase = uintOr(d, "phase");
    p.phase = phase <= static_cast<std::uint64_t>(flash::Phase::done)
                  ? static_cast<flash::Phase>(phase)
                  : flash::Phase::verifying;
    p.slot = getString(d, "slot");
    p.entry = getString(d, "entry");
    p.fileDone = uintOr(d, "fileDone");
    p.fileTotal = uintOr(d, "fileTotal");
    p.totalDone = uintOr(d, "totalDone");
    p.total = uintOr(d, "total");
    return p;
}

Xpc encodePlan(const PlanSummary& plan)
{
    auto d = Xpc::adopt(xpc_dictionary_create(nullptr, nullptr, 0));
    auto files = Xpc::adopt(xpc_array_create(nullptr, 0));
    for (const auto& f : plan.files) {
        auto item = Xpc::adopt(xpc_dictionary_create(nullptr, nullptr, 0));
        putUint(item.get(), "slot", static_cast<std::uint64_t>(f.slot));
        xpc_dictionary_set_string(item.get(), "entry", f.entry.c_str());
        xpc_dictionary_set_string(item.get(), "partition", f.partition.c_str());
        putUint(item.get(), "partitionId", f.partitionId);
        putUint(item.get(), "size", f.size);
        xpc_dictionary_set_bool(item.get(), "lz4", f.lz4);
        xpc_array_append_value(files.get(), item.get());
    }
    putChild(d.get(), "files", files);
    auto skipped = Xpc::adopt(xpc_array_create(nullptr, 0));
    for (const auto& s : plan.skipped) {
        auto item = Xpc::adopt(xpc_dictionary_create(nullptr, nullptr, 0));
        putUint(item.get(), "slot", static_cast<std::uint64_t>(s.slot));
        xpc_dictionary_set_string(item.get(), "entry", s.entry.c_str());
        xpc_dictionary_set_string(item.get(), "reason", s.reason.c_str());
        xpc_array_append_value(skipped.get(), item.get());
    }
    putChild(d.get(), "skipped", skipped);
    auto warnings = Xpc::adopt(xpc_array_create(nullptr, 0));
    for (const auto& w : plan.warnings) {
        auto item = Xpc::adopt(xpc_dictionary_create(nullptr, nullptr, 0));
        xpc_dictionary_set_string(item.get(), "message", w.message.c_str());
        xpc_dictionary_set_bool(item.get(), "ack", w.requiresAcknowledgement);
        xpc_array_append_value(warnings.get(), item.get());
    }
    putChild(d.get(), "warnings", warnings);
    putChild(d.get(), "refusals", stringArray(plan.refusals));
    xpc_dictionary_set_string(d.get(), "model", plan.detectedModel.c_str());
    xpc_dictionary_set_bool(d.get(), "erasesUserData", plan.erasesUserData);
    if (plan.bootloaderRevision) {
        xpc_dictionary_set_int64(d.get(), "blRevision", *plan.bootloaderRevision);
    }
    xpc_dictionary_set_bool(d.get(), "requiresAck", plan.requiresAcknowledgement);
    xpc_dictionary_set_bool(d.get(), "flashable", plan.flashable);
    return d;
}

PlanSummary decodePlan(xpc_object_t d)
{
    PlanSummary plan;
    forEachDictionary(d, "files", [&](xpc_object_t item) {
        plan.files.push_back({slotFrom(uintOr(item, "slot")), getString(item, "entry"),
                              getString(item, "partition"),
                              static_cast<std::uint32_t>(uintOr(item, "partitionId")),
                              uintOr(item, "size"), getBool(item, "lz4")});
    });
    forEachDictionary(d, "skipped", [&](xpc_object_t item) {
        plan.skipped.push_back(
            {slotFrom(uintOr(item, "slot")), getString(item, "entry"), getString(item, "reason")});
    });
    forEachDictionary(d, "warnings", [&](xpc_object_t item) {
        plan.warnings.push_back({getString(item, "message"), getBool(item, "ack")});
    });
    plan.refusals = getStringArray(d, "refusals");
    plan.detectedModel = getString(d, "model");
    plan.erasesUserData = getBool(d, "erasesUserData");
    if (auto revision = getInt(d, "blRevision")) {
        plan.bootloaderRevision = static_cast<int>(*revision);
    }
    plan.requiresAcknowledgement = getBool(d, "requiresAck");
    plan.flashable = getBool(d, "flashable");
    return plan;
}

Xpc encodeResult(const JobResult& result)
{
    auto d = Xpc::adopt(xpc_dictionary_create(nullptr, nullptr, 0));
    xpc_dictionary_set_bool(d.get(), "ok", !result.error.has_value());
    if (result.error) {
        putChild(d.get(), "error", encodeError(*result.error));
    }
    putChild(d.get(), "session", encodeSessionInfo(result.session));
    putUint(d.get(), "bytes", result.bytesTransferred);
    xpc_dictionary_set_bool(d.get(), "rebooted", result.rebooted);
    putData(d.get(), "pit", result.pit.data(), result.pit.size());
    return d;
}

JobResult decodeResult(xpc_object_t d)
{
    JobResult result;
    if (!getBool(d, "ok")) {
        result.error = decodeError(child(d, "error", XPC_TYPE_DICTIONARY));
        if (!result.error) {
            result.error = makeError(ErrorCode::protocolViolation, "Result had no error detail");
            result.error->dataState = DataState::partial;
        }
    }
    if (auto session = child(d, "session", XPC_TYPE_DICTIONARY)) {
        result.session = decodeSessionInfo(session);
    }
    result.bytesTransferred = uintOr(d, "bytes");
    result.rebooted = getBool(d, "rebooted");
    result.pit = getData(d, "pit");
    return result;
}

const char* toString(JobKind kind) noexcept
{
    switch (kind) {
    case JobKind::flash: return "flash";
    case JobKind::probe: return "probe";
    case JobKind::readPit: return "read-pit";
    }
    return "unknown";
}

std::optional<JobKind> parseJobKind(const std::string& text)
{
    for (auto kind : {JobKind::flash, JobKind::probe, JobKind::readPit}) {
        if (text == toString(kind)) {
            return kind;
        }
    }
    return std::nullopt;
}

Xpc encodeSnapshot(const JobSnapshot& s)
{
    auto d = Xpc::adopt(xpc_dictionary_create(nullptr, nullptr, 0));
    putUint(d.get(), "id", s.id);
    xpc_dictionary_set_string(d.get(), "kind", toString(s.kind));
    xpc_dictionary_set_bool(d.get(), "running", s.running);
    auto device = Xpc::adopt(xpc_dictionary_create(nullptr, nullptr, 0));
    putIdentity(device.get(), s.device);
    putChild(d.get(), "device", device);
    putChild(d.get(), "progress", encodeProgress(s.progress));
    if (s.plan) {
        putChild(d.get(), "plan", encodePlan(*s.plan));
    }
    auto log = Xpc::adopt(xpc_array_create(nullptr, 0));
    for (const auto& line : s.log) {
        auto item = Xpc::adopt(xpc_dictionary_create(nullptr, nullptr, 0));
        putUint(item.get(), "level", static_cast<std::uint64_t>(line.level));
        xpc_dictionary_set_string(item.get(), "message", line.message.c_str());
        xpc_array_append_value(log.get(), item.get());
    }
    putChild(d.get(), "log", log);
    if (s.result) {
        putChild(d.get(), "result", encodeResult(*s.result));
    }
    return d;
}

JobSnapshot decodeSnapshot(xpc_object_t d)
{
    JobSnapshot s;
    s.id = uintOr(d, "id");
    s.kind = parseJobKind(getString(d, "kind")).value_or(JobKind::flash);
    s.running = getBool(d, "running");
    if (auto device = child(d, "device", XPC_TYPE_DICTIONARY)) {
        s.device = getIdentity(device).value_or(transport::UsbIdentity{});
    }
    if (auto progress = child(d, "progress", XPC_TYPE_DICTIONARY)) {
        s.progress = decodeProgress(progress);
    }
    if (auto plan = child(d, "plan", XPC_TYPE_DICTIONARY)) {
        s.plan = decodePlan(plan);
    }
    forEachDictionary(d, "log", [&](xpc_object_t item) {
        const auto level = std::min<std::uint64_t>(uintOr(item, "level"), 2);
        s.log.push_back({static_cast<flash::LogLevel>(level), getString(item, "message")});
    });
    if (auto result = child(d, "result", XPC_TYPE_DICTIONARY)) {
        s.result = decodeResult(result);
    }
    return s;
}

} // namespace bifrost::service
