#include "bifrost/service/journal.hpp"

#include "bifrost/common/error.hpp"
#include "bifrost/flash/flash.hpp"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace bifrost::service {
namespace {

constexpr unsigned kJournalVersion = 2;

std::string journalPath()
{
    const char* home = std::getenv("HOME");
    if (home == nullptr || home[0] == '\0') {
        return {};
    }
    return std::string(home) + "/Library/Application Support/Bifrost Studio/agent-journal.json";
}

bool ensureParent(const std::string& path)
{
    const auto slash = path.rfind('/');
    if (slash == std::string::npos) {
        return false;
    }
    std::error_code ignored;
    std::filesystem::create_directories(path.substr(0, slash), ignored);
    return !ignored;
}

std::string jsonEscape(std::string_view text)
{
    std::string out;
    out.reserve(text.size() + 8);
    for (unsigned char c : text) {
        if (c == '"' || c == '\\') {
            out.push_back('\\');
        }
        out.push_back(static_cast<char>(c));
    }
    return out;
}

std::optional<std::uint64_t> scanUint64(const std::string& json, const char* key)
{
    const std::string needle = std::string("\"") + key + "\":";
    const auto pos = json.find(needle);
    if (pos == std::string::npos) {
        return std::nullopt;
    }
    const char* start = json.c_str() + pos + needle.size();
    char* end = nullptr;
    errno = 0;
    const auto value = std::strtoull(start, &end, 10);
    if (errno != 0 || end == start) {
        return std::nullopt;
    }
    return value;
}

std::optional<bool> scanBool(const std::string& json, const char* key)
{
    const std::string needle = std::string("\"") + key + "\":";
    const auto pos = json.find(needle);
    if (pos == std::string::npos) {
        return std::nullopt;
    }
    const auto after = json.substr(pos + needle.size());
    if (after.compare(0, 4, "true") == 0) {
        return true;
    }
    if (after.compare(0, 5, "false") == 0) {
        return false;
    }
    return std::nullopt;
}

std::optional<std::string> scanQuotedString(const std::string& json, const char* key)
{
    const std::string needle = std::string("\"") + key + "\":\"";
    const auto pos = json.find(needle);
    if (pos == std::string::npos) {
        return std::nullopt;
    }
    const auto from = pos + needle.size();
    const auto end = json.find('"', from);
    if (end == std::string::npos) {
        return std::nullopt;
    }
    return json.substr(from, end - from);
}

std::string readFile(const std::string& path)
{
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return {};
    }
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

flash::Phase parsePhase(const std::string& text)
{
    using flash::Phase;
    if (text == "verifying") {
        return Phase::verifying;
    }
    if (text == "checking-images") {
        return Phase::checkingImages;
    }
    if (text == "connecting") {
        return Phase::connecting;
    }
    if (text == "reading-pit") {
        return Phase::readingPit;
    }
    if (text == "planning") {
        return Phase::planning;
    }
    if (text == "transferring") {
        return Phase::transferring;
    }
    if (text == "committing") {
        return Phase::committing;
    }
    if (text == "closing") {
        return Phase::closing;
    }
    if (text == "done") {
        return Phase::done;
    }
    return Phase::verifying;
}

JobResult crashResult(const std::string& json)
{
    JobResult recovered;
    Error err = makeError(ErrorCode::serviceUnavailable,
                          "The agent stopped unexpectedly. The phone may have been "
                          "partially written.");
    if (const auto code = scanUint64(json, "errorCode")) {
        err.code = static_cast<ErrorCode>(*code);
    } else {
        err.dataState = DataState::partial;
    }
    if (const auto state = scanUint64(json, "dataState")) {
        err.dataState = static_cast<DataState>(*state);
    }
    if (const auto msg = scanQuotedString(json, "lastLog")) {
        if (!msg->empty()) {
            err.message += " Last log line: " + *msg;
        }
    }
    recovered.error = std::move(err);
    return recovered;
}

} // namespace

void journalPersist(const JobSnapshot& snapshot)
{
    const auto path = journalPath();
    if (path.empty() || !ensureParent(path)) {
        return;
    }
    if (!snapshot.running) {
        return;
    }
    const std::string temp = path + ".tmp";
    std::ofstream out(temp, std::ios::trunc);
    if (!out) {
        return;
    }
    out << "{\"version\":" << kJournalVersion << ",\"id\":" << snapshot.id << ",\"kind\":\""
        << toString(snapshot.kind) << "\",\"running\":true";
    out << ",\"phase\":\"" << flash::toString(snapshot.progress.phase) << "\"";
    out << ",\"progressDone\":" << snapshot.progress.totalDone;
    out << ",\"progressTotal\":" << snapshot.progress.total;
    if (snapshot.device.vendorId != 0 || snapshot.device.productId != 0) {
        out << ",\"vendorId\":" << snapshot.device.vendorId;
        out << ",\"productId\":" << snapshot.device.productId;
        out << ",\"bus\":" << static_cast<unsigned>(snapshot.device.busNumber);
        out << ",\"address\":" << static_cast<unsigned>(snapshot.device.deviceAddress);
    }
    if (!snapshot.log.empty()) {
        out << ",\"lastLog\":\"" << jsonEscape(snapshot.log.back().message) << "\"";
    }
    out << '}';
    out.flush();
    if (!out) {
        unlink(temp.c_str());
        return;
    }
    rename(temp.c_str(), path.c_str());
}

std::optional<JobSnapshot> journalRecover()
{
    const auto path = journalPath();
    const auto json = readFile(path);
    if (json.empty()) {
        return std::nullopt;
    }
    const auto version = scanUint64(json, "version");
    const auto id = scanUint64(json, "id");
    const auto running = scanBool(json, "running");
    if (!version || !id || !running || !*running) {
        return std::nullopt;
    }
    JobSnapshot snapshot;
    snapshot.id = *id;
    if (const auto kind = scanQuotedString(json, "kind")) {
        if (const auto parsed = parseJobKind(*kind)) {
            snapshot.kind = *parsed;
        }
    }
    if (const auto phase = scanQuotedString(json, "phase")) {
        snapshot.progress.phase = parsePhase(*phase);
    }
    if (const auto done = scanUint64(json, "progressDone")) {
        snapshot.progress.totalDone = *done;
    }
    if (const auto total = scanUint64(json, "progressTotal")) {
        snapshot.progress.total = *total;
    }
    if (const auto vid = scanUint64(json, "vendorId")) {
        snapshot.device.vendorId = static_cast<std::uint16_t>(*vid);
    }
    if (const auto pid = scanUint64(json, "productId")) {
        snapshot.device.productId = static_cast<std::uint16_t>(*pid);
    }
    if (const auto bus = scanUint64(json, "bus")) {
        snapshot.device.busNumber = static_cast<std::uint8_t>(*bus);
    }
    if (const auto addr = scanUint64(json, "address")) {
        snapshot.device.deviceAddress = static_cast<std::uint8_t>(*addr);
    }
    if (const auto line = scanQuotedString(json, "lastLog")) {
        snapshot.log.push_back({flash::LogLevel::warning, *line});
    }
    snapshot.running = false;
    snapshot.result = crashResult(json);
    (void)version;
    return snapshot;
}

void journalClear()
{
    const auto path = journalPath();
    if (!path.empty()) {
        unlink(path.c_str());
    }
}

} // namespace bifrost::service