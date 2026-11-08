// C API over the C++ core. Every entry point catches exceptions; nothing
// propagates across the C boundary.
#include "bifrost/bifrost.h"

#include "capi/capi_internal.hpp"
#include "capi/capi_types.hpp"

#include "bifrost/common/cancel.hpp"
#include "bifrost/firmware/naming.hpp"
#include "bifrost/firmware/package.hpp"
#include "bifrost/flash/flash.hpp"
#include "bifrost/pit/pit.hpp"
#include "bifrost/planner/plan.hpp"
#include "bifrost/version.h"

#include <array>
#include <atomic>
#include <cctype>
#include <cstring>
#include <fstream>
#include <mutex>
#include <new>
#include <string>
#include <vector>

using namespace bifrost;

// ---- ABI checks: the C enums mirror the C++ enums value for value ----------

static_assert(static_cast<int>(ErrorCode::invalidArgument) == BF_E_INVALID_ARGUMENT);
static_assert(static_cast<int>(ErrorCode::ioError) == BF_E_IO);
static_assert(static_cast<int>(ErrorCode::packageInvalid) == BF_E_PACKAGE_INVALID);
static_assert(static_cast<int>(ErrorCode::checksumMismatch) == BF_E_CHECKSUM_MISMATCH);
static_assert(static_cast<int>(ErrorCode::archiveUnsupported) == BF_E_ARCHIVE_UNSUPPORTED);
static_assert(static_cast<int>(ErrorCode::lz4Invalid) == BF_E_LZ4_INVALID);
static_assert(static_cast<int>(ErrorCode::insufficientSpace) == BF_E_INSUFFICIENT_SPACE);
static_assert(static_cast<int>(ErrorCode::fileChanged) == BF_E_FILE_CHANGED);
static_assert(static_cast<int>(ErrorCode::pitInvalid) == BF_E_PIT_INVALID);
static_assert(static_cast<int>(ErrorCode::planRefused) == BF_E_PLAN_REFUSED);
static_assert(static_cast<int>(ErrorCode::modelMismatch) == BF_E_MODEL_MISMATCH);
static_assert(static_cast<int>(ErrorCode::usbDisconnected) == BF_E_USB_DISCONNECTED);
static_assert(static_cast<int>(ErrorCode::usbTimeout) == BF_E_USB_TIMEOUT);
static_assert(static_cast<int>(ErrorCode::usbStall) == BF_E_USB_STALL);
static_assert(static_cast<int>(ErrorCode::usbAccess) == BF_E_USB_ACCESS);
static_assert(static_cast<int>(ErrorCode::usbOther) == BF_E_USB_OTHER);
static_assert(static_cast<int>(ErrorCode::protocolViolation) == BF_E_PROTOCOL_VIOLATION);
static_assert(static_cast<int>(ErrorCode::bootloaderRejected) == BF_E_BOOTLOADER_REJECTED);
static_assert(static_cast<int>(ErrorCode::cancelled) == BF_E_CANCELLED);
static_assert(static_cast<int>(ErrorCode::illegalState) == BF_E_ILLEGAL_STATE);
static_assert(static_cast<int>(ErrorCode::serviceUnavailable) == BF_E_SERVICE_UNAVAILABLE);

static_assert(static_cast<int>(DataState::none) == BF_DATA_NONE);
static_assert(static_cast<int>(DataState::partial) == BF_DATA_PARTIAL);
static_assert(static_cast<int>(DataState::complete) == BF_DATA_COMPLETE);

static_assert(static_cast<int>(flash::Phase::verifying) == BF_PHASE_VERIFYING);
static_assert(static_cast<int>(flash::Phase::checkingImages) == BF_PHASE_CHECKING_IMAGES);
static_assert(static_cast<int>(flash::Phase::connecting) == BF_PHASE_CONNECTING);
static_assert(static_cast<int>(flash::Phase::readingPit) == BF_PHASE_READING_PIT);
static_assert(static_cast<int>(flash::Phase::planning) == BF_PHASE_PLANNING);
static_assert(static_cast<int>(flash::Phase::transferring) == BF_PHASE_TRANSFERRING);
static_assert(static_cast<int>(flash::Phase::committing) == BF_PHASE_COMMITTING);
static_assert(static_cast<int>(flash::Phase::closing) == BF_PHASE_CLOSING);
static_assert(static_cast<int>(flash::Phase::done) == BF_PHASE_DONE);

static_assert(static_cast<int>(flash::LogLevel::info) == BF_LOG_INFO);
static_assert(static_cast<int>(flash::LogLevel::warning) == BF_LOG_WARNING);
static_assert(static_cast<int>(flash::LogLevel::error) == BF_LOG_ERROR);

static_assert(static_cast<int>(planner::Slot::bl) == BF_SLOT_BL);
static_assert(static_cast<int>(planner::Slot::ap) == BF_SLOT_AP);
static_assert(static_cast<int>(planner::Slot::cp) == BF_SLOT_CP);
static_assert(static_cast<int>(planner::Slot::csc) == BF_SLOT_CSC);
static_assert(static_cast<int>(planner::Slot::userdata) == BF_SLOT_USERDATA);

static_assert(static_cast<int>(firmware::PackageKind::unknown) == BF_KIND_UNKNOWN);
static_assert(static_cast<int>(firmware::PackageKind::bl) == BF_KIND_BL);
static_assert(static_cast<int>(firmware::PackageKind::ap) == BF_KIND_AP);
static_assert(static_cast<int>(firmware::PackageKind::cp) == BF_KIND_CP);
static_assert(static_cast<int>(firmware::PackageKind::csc) == BF_KIND_CSC);
static_assert(static_cast<int>(firmware::PackageKind::homeCsc) == BF_KIND_HOME_CSC);
static_assert(static_cast<int>(firmware::PackageKind::userdata) == BF_KIND_USERDATA);

static_assert(static_cast<int>(CancelLevel::none) == BF_CANCEL_NONE);
static_assert(static_cast<int>(CancelLevel::beforeData) == BF_CANCEL_BEFORE_DATA);
static_assert(static_cast<int>(CancelLevel::afterSequence) == BF_CANCEL_AFTER_SEQUENCE);

// ---- opaque types -----------------------------------------------------------

struct bf_ctx {
    std::unique_ptr<transport::UsbTransport> transport;
    CancelToken cancel;
    std::atomic<bool> busy{false};

    std::mutex callbackMutex;
    bf_log_fn log{};
    void* logUser{};
    bf_progress_fn progress{};
    void* progressUser{};
    bf_plan_fn plan{};
    void* planUser{};

    std::mutex hotplugMutex;
    bf_hotplug_fn hotplug{};
    void* hotplugUser{};
};

namespace {

using namespace bifrost::capi;

constexpr std::size_t kMaximumPitFileSize = 1024 * 1024;

// Marks a context busy for the duration of one long operation.
class BusyGuard {
public:
    explicit BusyGuard(bf_ctx* ctx) : ctx_(ctx)
    {
        bool expected = false;
        acquired_ = ctx_->busy.compare_exchange_strong(expected, true);
    }
    ~BusyGuard()
    {
        if (acquired_) {
            ctx_->busy.store(false);
        }
    }
    bool acquired() const noexcept { return acquired_; }

private:
    bf_ctx* ctx_;
    bool acquired_{};
};

bool requireArguments(bool valid, bf_error* error, const char* what)
{
    if (!valid) {
        setError(error, BF_E_INVALID_ARGUMENT, std::string("Invalid argument: ") + what);
    }
    return valid;
}

bool beginOperation(bf_ctx* ctx, const BusyGuard& guard, bf_error* error)
{
    if (!guard.acquired()) {
        setError(error, BF_E_ILLEGAL_STATE,
                 "Another operation is already running on this context");
        return false;
    }
    (void)ctx;
    clearError(error);
    return true;
}

bool requireTransport(bf_ctx* ctx, bf_error* error)
{
    if (!ctx->transport) {
        setError(error, BF_E_UNSUPPORTED, "This build has no USB support");
        return false;
    }
    return true;
}

flash::Observer observerFor(bf_ctx* ctx)
{
    bf_log_fn log;
    void* logUser;
    bf_progress_fn progress;
    void* progressUser;
    bf_plan_fn plan;
    void* planUser;
    {
        std::lock_guard<std::mutex> lock(ctx->callbackMutex);
        log = ctx->log;
        logUser = ctx->logUser;
        progress = ctx->progress;
        progressUser = ctx->progressUser;
        plan = ctx->plan;
        planUser = ctx->planUser;
    }
    flash::Observer observer;
    if (log != nullptr) {
        observer.onLog = [log, logUser](flash::LogLevel level, const std::string& text) {
            log(logUser, static_cast<std::int32_t>(level), text.c_str());
        };
    }
    if (progress != nullptr) {
        observer.onProgress = [progress, progressUser](const flash::Progress& p) {
            const bf_progress c{static_cast<std::int32_t>(p.phase),
                                p.slot.c_str(),
                                p.entry.c_str(),
                                p.fileDone,
                                p.fileTotal,
                                p.totalDone,
                                p.total};
            progress(progressUser, &c);
        };
    }
    if (plan != nullptr) {
        observer.onPlan = [plan, planUser](const planner::FlashPlan& built) {
            bf_plan wrapper = makePlan(built);
            plan(planUser, &wrapper);
        };
    }
    return observer;
}

Expected<std::unique_ptr<transport::ClaimedInterface>> claim(bf_ctx* ctx,
                                                             const bf_device_id& device)
{
    const transport::UsbIdentity identity{device.vendor_id, device.product_id, device.bus,
                                          device.address};
    if (identity.vendorId != transport::kSamsungVendorId) {
        return makeError(ErrorCode::invalidArgument, "Not a Samsung USB vendor ID");
    }
    return ctx->transport->claim(identity);
}

bool validSlot(std::int32_t slot)
{
    return slot >= BF_SLOT_BL && slot <= BF_SLOT_USERDATA;
}

} // namespace

namespace bifrost::capi {

bf_ctx* createContext(std::unique_ptr<transport::UsbTransport> transport)
{
    auto* ctx = new (std::nothrow) bf_ctx;
    if (ctx != nullptr) {
        ctx->transport = std::move(transport);
    }
    return ctx;
}

} // namespace bifrost::capi

// ---- version and names ------------------------------------------------------

extern "C" {

uint32_t bf_api_version(void) { return BF_API_VERSION; }

const char* bf_version(void) { return bifrost_core_version(); }

const char* bf_code_name(int32_t code)
{
    if (code == BF_OK) {
        return "ok";
    }
    if (code == BF_E_UNSUPPORTED) {
        return "unsupported";
    }
    if (code >= BF_E_INVALID_ARGUMENT && code <= BF_E_SERVICE_UNAVAILABLE) {
        return toString(static_cast<ErrorCode>(code));
    }
    return "unknown";
}

const char* bf_data_state_name(int32_t state)
{
    switch (state) {
    case BF_DATA_NONE: return "none";
    case BF_DATA_PARTIAL: return "partial";
    case BF_DATA_COMPLETE: return "complete";
    default: return "unknown";
    }
}

// ---- context ----------------------------------------------------------------

bf_ctx* bf_create(bf_error* error)
{
    return guarded(error, static_cast<bf_ctx*>(nullptr), [&]() -> bf_ctx* {
        auto transport = capi::makeDefaultTransport();
        if (!transport) {
            setError(error, transport.error());
            return nullptr;
        }
        auto* ctx = capi::createContext(std::move(*transport));
        if (ctx == nullptr) {
            setError(error, BF_E_IO, "Out of memory");
            return nullptr;
        }
        clearError(error);
        return ctx;
    });
}

void bf_destroy(bf_ctx* ctx)
{
    if (ctx == nullptr) {
        return;
    }
    try {
        if (ctx->transport) {
            ctx->transport->stopHotplug();
        }
    } catch (...) {
    }
    delete ctx;
}

void bf_set_log_callback(bf_ctx* ctx, bf_log_fn fn, void* user)
{
    if (ctx != nullptr) {
        std::lock_guard<std::mutex> lock(ctx->callbackMutex);
        ctx->log = fn;
        ctx->logUser = user;
    }
}

void bf_set_progress_callback(bf_ctx* ctx, bf_progress_fn fn, void* user)
{
    if (ctx != nullptr) {
        std::lock_guard<std::mutex> lock(ctx->callbackMutex);
        ctx->progress = fn;
        ctx->progressUser = user;
    }
}

void bf_set_plan_callback(bf_ctx* ctx, bf_plan_fn fn, void* user)
{
    if (ctx != nullptr) {
        std::lock_guard<std::mutex> lock(ctx->callbackMutex);
        ctx->plan = fn;
        ctx->planUser = user;
    }
}

void bf_cancel(bf_ctx* ctx, int32_t level)
{
    if (ctx != nullptr && level >= BF_CANCEL_BEFORE_DATA && level <= BF_CANCEL_AFTER_SEQUENCE) {
        ctx->cancel.request(static_cast<CancelLevel>(level));
    }
}

void bf_cancel_reset(bf_ctx* ctx)
{
    if (ctx != nullptr) {
        ctx->cancel.reset();
    }
}

// ---- devices ----------------------------------------------------------------

bf_device_list* bf_devices(bf_ctx* ctx, bf_error* error)
{
    return guarded(error, static_cast<bf_device_list*>(nullptr), [&]() -> bf_device_list* {
        if (!requireArguments(ctx != nullptr, error, "ctx") || !requireTransport(ctx, error)) {
            return nullptr;
        }
        auto enumeration = ctx->transport->enumerate();
        if (!enumeration) {
            setError(error, enumeration.error());
            return nullptr;
        }
        clearError(error);
        return new bf_device_list{std::move(*enumeration)};
    });
}

size_t bf_device_list_count(const bf_device_list* list)
{
    return list == nullptr ? 0 : list->enumeration.devices.size();
}

bool bf_device_list_get(const bf_device_list* list, size_t index, bf_device_info* out)
{
    if (list == nullptr || out == nullptr || index >= list->enumeration.devices.size()) {
        return false;
    }
    const auto& device = list->enumeration.devices[index];
    *out = {};
    out->id = {device.identity.vendorId, device.identity.productId,
               device.identity.busNumber, device.identity.deviceAddress};
    if (device.odinInterface) {
        out->has_odin_interface = true;
        out->interface_number = device.odinInterface->interfaceNumber;
        out->alternate_setting = device.odinInterface->alternateSetting;
        out->endpoint_in = device.odinInterface->endpointIn;
        out->endpoint_out = device.odinInterface->endpointOut;
    }
    return true;
}

size_t bf_device_list_warning_count(const bf_device_list* list)
{
    return list == nullptr ? 0 : list->enumeration.warnings.size();
}

const char* bf_device_list_warning(const bf_device_list* list, size_t index)
{
    if (list == nullptr || index >= list->enumeration.warnings.size()) {
        return nullptr;
    }
    return list->enumeration.warnings[index].c_str();
}

void bf_device_list_free(bf_device_list* list) { delete list; }

bool bf_start_hotplug(bf_ctx* ctx, bf_hotplug_fn fn, void* user)
{
    if (ctx == nullptr || fn == nullptr || !ctx->transport) {
        return false;
    }
    try {
        {
            std::lock_guard<std::mutex> lock(ctx->hotplugMutex);
            ctx->hotplug = fn;
            ctx->hotplugUser = user;
        }
        return ctx->transport->startHotplug([ctx] {
            bf_hotplug_fn callback;
            void* callbackUser;
            {
                std::lock_guard<std::mutex> lock(ctx->hotplugMutex);
                callback = ctx->hotplug;
                callbackUser = ctx->hotplugUser;
            }
            if (callback != nullptr) {
                callback(callbackUser);
            }
        });
    } catch (...) {
        return false;
    }
}

void bf_stop_hotplug(bf_ctx* ctx)
{
    if (ctx == nullptr || !ctx->transport) {
        return;
    }
    try {
        ctx->transport->stopHotplug();
    } catch (...) {
    }
    std::lock_guard<std::mutex> lock(ctx->hotplugMutex);
    ctx->hotplug = nullptr;
    ctx->hotplugUser = nullptr;
}

// ---- packages ---------------------------------------------------------------

bf_packages* bf_packages_load(bf_ctx* ctx, const bf_slot_path* paths, size_t count,
                              bf_error* error)
{
    return guarded(error, static_cast<bf_packages*>(nullptr), [&]() -> bf_packages* {
        if (!requireArguments(ctx != nullptr && paths != nullptr && count > 0, error,
                              "ctx, paths and count are required")) {
            return nullptr;
        }
        std::vector<flash::SlotPath> slots;
        for (size_t i = 0; i < count; ++i) {
            if (!requireArguments(validSlot(paths[i].slot) && paths[i].path != nullptr &&
                                      paths[i].path[0] != '\0',
                                  error, "each entry needs a valid slot and path")) {
                return nullptr;
            }
            slots.push_back({static_cast<planner::Slot>(paths[i].slot), paths[i].path});
        }
        BusyGuard guard(ctx);
        if (!beginOperation(ctx, guard, error)) {
            return nullptr;
        }
        auto loaded = flash::loadPackages(slots, observerFor(ctx), &ctx->cancel);
        if (!loaded) {
            setError(error, loaded.error());
            return nullptr;
        }
        auto* result = new bf_packages;
        result->packages = std::move(*loaded);
        for (const auto& package : result->packages) {
            result->names.push_back(firmware::parseFirmwareName(package.package->path));
        }
        return result;
    });
}

size_t bf_packages_count(const bf_packages* packages)
{
    return packages == nullptr ? 0 : packages->packages.size();
}

bool bf_packages_get(const bf_packages* packages, size_t index, bf_package_info* out)
{
    if (packages == nullptr || out == nullptr || index >= packages->packages.size()) {
        return false;
    }
    const auto& slot = packages->packages[index];
    const auto& package = *slot.package;
    const auto& name = packages->names[index];
    *out = {};
    out->slot = static_cast<std::int32_t>(slot.slot);
    out->path = package.path.c_str();
    out->archive_size = package.archiveSize;
    out->checksum_present = package.checksumPresent;
    out->checksum_valid = package.checksumValid;
    out->checksum_hex = package.checksumHex.c_str();
    out->entry_count = package.entries.size();
    out->kind = static_cast<std::int32_t>(name.kind);
    out->build = name.build.c_str();
    out->model = name.modelCode.c_str();
    out->csc_code = name.cscCode.c_str();
    out->bootloader_revision = name.bootloaderRevision.value_or(-1);
    return true;
}

bool bf_packages_entry(const bf_packages* packages, size_t package_index,
                       size_t entry_index, bf_entry_info* out)
{
    if (packages == nullptr || out == nullptr || package_index >= packages->packages.size()) {
        return false;
    }
    const auto& entries = packages->packages[package_index].package->entries;
    if (entry_index >= entries.size()) {
        return false;
    }
    const auto& entry = entries[entry_index];
    *out = {entry.name.c_str(), entry.size, entry.lz4Compressed};
    return true;
}

void bf_packages_free(bf_packages* packages) { delete packages; }

// ---- PIT --------------------------------------------------------------------

bf_pit* bf_pit_parse(const uint8_t* bytes, size_t size, bf_error* error)
{
    return guarded(error, static_cast<bf_pit*>(nullptr), [&]() -> bf_pit* {
        if (!requireArguments(bytes != nullptr && size > 0, error, "PIT bytes")) {
            return nullptr;
        }
        auto table = pit::parse(bytes, size);
        if (!table) {
            setError(error, table.error());
            return nullptr;
        }
        clearError(error);
        return new bf_pit{std::vector<std::uint8_t>(bytes, bytes + size), std::move(*table)};
    });
}

bf_pit* bf_packages_embedded_pit(const bf_packages* packages, bf_error* error)
{
    return guarded(error, static_cast<bf_pit*>(nullptr), [&]() -> bf_pit* {
        if (!requireArguments(packages != nullptr, error, "packages")) {
            return nullptr;
        }
        const planner::SlotPackage* owner = nullptr;
        const firmware::ArchiveEntry* found = nullptr;
        for (const auto& item : packages->packages) {
            for (const auto& entry : item.package->entries) {
                if (entry.name.find('/') != std::string::npos || entry.name.size() < 4) {
                    continue;
                }
                std::string suffix = entry.name.substr(entry.name.size() - 4);
                for (auto& c : suffix) {
                    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                }
                if (suffix != ".pit") {
                    continue;
                }
                if (found != nullptr) {
                    setError(error, BF_E_PIT_INVALID,
                             "The firmware contains more than one PIT (" + found->name + ", " +
                                 entry.name + "); open the right one as a file instead");
                    return nullptr;
                }
                owner = &item;
                found = &entry;
            }
        }
        if (found == nullptr) {
            setError(error, BF_E_PIT_INVALID,
                     "The selected firmware has no PIT (it is normally inside CSC or "
                     "HOME_CSC)");
            return nullptr;
        }
        if (found->size == 0 || found->size > kMaximumPitFileSize) {
            setError(error, BF_E_PIT_INVALID, "Firmware PIT " + found->name + " has an invalid size");
            return nullptr;
        }
        std::vector<std::uint8_t> bytes(static_cast<std::size_t>(found->size));
        if (auto read = firmware::readEntryBytes(*owner->package, *found, 0, bytes.data(),
                                                 bytes.size());
            !read) {
            setError(error, read.error());
            return nullptr;
        }
        auto table = pit::parse(bytes.data(), bytes.size());
        if (!table) {
            Error failed = std::move(table).error();
            failed.message = "Firmware PIT " + found->name + ": " + failed.message;
            setError(error, failed);
            return nullptr;
        }
        clearError(error);
        return new bf_pit{std::move(bytes), std::move(*table)};
    });
}

bf_pit* bf_pit_load_file(const char* path, bf_error* error)
{
    return guarded(error, static_cast<bf_pit*>(nullptr), [&]() -> bf_pit* {
        if (!requireArguments(path != nullptr && path[0] != '\0', error, "path")) {
            return nullptr;
        }
        std::ifstream file(path, std::ios::binary);
        if (!file) {
            setError(error, BF_E_IO, std::string("Could not open ") + path);
            return nullptr;
        }
        std::vector<std::uint8_t> bytes;
        std::array<char, 65536> buffer{};
        while (file) {
            file.read(buffer.data(), buffer.size());
            const auto got = static_cast<std::size_t>(file.gcount());
            if (bytes.size() + got > kMaximumPitFileSize) {
                setError(error, BF_E_PIT_INVALID, "PIT file is larger than 1 MiB");
                return nullptr;
            }
            bytes.insert(bytes.end(), buffer.data(), buffer.data() + got);
        }
        if (file.bad()) {
            setError(error, BF_E_IO, std::string("Could not read ") + path);
            return nullptr;
        }
        return bf_pit_parse(bytes.data(), bytes.size(), error);
    });
}

bf_pit* bf_read_pit(bf_ctx* ctx, const bf_device_id* device, bool resume, bf_error* error)
{
    return guarded(error, static_cast<bf_pit*>(nullptr), [&]() -> bf_pit* {
        if (!requireArguments(ctx != nullptr && device != nullptr, error, "ctx and device") ||
            !requireTransport(ctx, error)) {
            return nullptr;
        }
        BusyGuard guard(ctx);
        if (!beginOperation(ctx, guard, error)) {
            return nullptr;
        }
        auto interface = claim(ctx, *device);
        if (!interface) {
            setError(error, interface.error());
            return nullptr;
        }
        auto pit = flash::readDevicePit(**interface, {}, resume);
        if (!pit) {
            setError(error, pit.error());
            return nullptr;
        }
        return new bf_pit{std::move(pit->bytes), std::move(pit->table)};
    });
}

const uint8_t* bf_pit_bytes(const bf_pit* pit, size_t* size)
{
    if (pit == nullptr) {
        if (size != nullptr) {
            *size = 0;
        }
        return nullptr;
    }
    if (size != nullptr) {
        *size = pit->bytes.size();
    }
    return pit->bytes.data();
}

size_t bf_pit_entry_count(const bf_pit* pit)
{
    return pit == nullptr ? 0 : pit->table.entries.size();
}

bool bf_pit_entry_get(const bf_pit* pit, size_t index, bf_pit_entry* out)
{
    if (pit == nullptr || out == nullptr || index >= pit->table.entries.size()) {
        return false;
    }
    const auto& e = pit->table.entries[index];
    *out = {e.binaryType,         e.deviceType,         e.identifier,
            e.attributes,         e.updateAttributes,   e.blockSizeOrOffset,
            e.blockCount,         e.fileOffset,         e.fileSize,
            e.partitionName.c_str(), e.flashFilename.c_str(), e.fotaFilename.c_str()};
    return true;
}

void bf_pit_free(bf_pit* pit) { delete pit; }

// ---- plans ------------------------------------------------------------------

bf_plan* bf_plan_offline(const bf_pit* pit, const bf_packages* packages, bf_error* error)
{
    return guarded(error, static_cast<bf_plan*>(nullptr), [&]() -> bf_plan* {
        if (!requireArguments(pit != nullptr && packages != nullptr, error,
                              "pit and packages")) {
            return nullptr;
        }
        clearError(error);
        return new bf_plan(makePlan(planner::buildPlan(pit->table, packages->packages)));
    });
}

bool bf_plan_flashable(const bf_plan* plan) { return plan != nullptr && plan->summary.flashable; }

bool bf_plan_requires_acknowledgement(const bf_plan* plan)
{
    return plan != nullptr && plan->summary.requiresAcknowledgement;
}

bool bf_plan_erases_user_data(const bf_plan* plan)
{
    return plan != nullptr && plan->summary.erasesUserData;
}

const char* bf_plan_detected_model(const bf_plan* plan)
{
    return plan == nullptr ? "" : plan->summary.detectedModel.c_str();
}

int32_t bf_plan_bootloader_revision(const bf_plan* plan)
{
    return plan == nullptr ? -1 : plan->summary.bootloaderRevision.value_or(-1);
}

size_t bf_plan_file_count(const bf_plan* plan)
{
    return plan == nullptr ? 0 : plan->summary.files.size();
}

bool bf_plan_file(const bf_plan* plan, size_t index, bf_planned_file* out)
{
    if (plan == nullptr || out == nullptr || index >= plan->summary.files.size()) {
        return false;
    }
    const auto& f = plan->summary.files[index];
    *out = {static_cast<std::int32_t>(f.slot), f.entry.c_str(), f.partition.c_str(),
            f.partitionId, f.size, f.lz4};
    return true;
}

size_t bf_plan_skipped_count(const bf_plan* plan)
{
    return plan == nullptr ? 0 : plan->summary.skipped.size();
}

bool bf_plan_skipped(const bf_plan* plan, size_t index, bf_skipped_entry* out)
{
    if (plan == nullptr || out == nullptr || index >= plan->summary.skipped.size()) {
        return false;
    }
    const auto& s = plan->summary.skipped[index];
    *out = {static_cast<std::int32_t>(s.slot), s.entry.c_str(), s.reason.c_str()};
    return true;
}

size_t bf_plan_warning_count(const bf_plan* plan)
{
    return plan == nullptr ? 0 : plan->summary.warnings.size();
}

const char* bf_plan_warning(const bf_plan* plan, size_t index, bool* requires_acknowledgement)
{
    if (plan == nullptr || index >= plan->summary.warnings.size()) {
        return nullptr;
    }
    const auto& w = plan->summary.warnings[index];
    if (requires_acknowledgement != nullptr) {
        *requires_acknowledgement = w.requiresAcknowledgement;
    }
    return w.message.c_str();
}

size_t bf_plan_refusal_count(const bf_plan* plan)
{
    return plan == nullptr ? 0 : plan->summary.refusals.size();
}

const char* bf_plan_refusal(const bf_plan* plan, size_t index)
{
    if (plan == nullptr || index >= plan->summary.refusals.size()) {
        return nullptr;
    }
    return plan->summary.refusals[index].c_str();
}

void bf_plan_free(bf_plan* plan) { delete plan; }

// ---- device sessions --------------------------------------------------------

bool bf_probe(bf_ctx* ctx, const bf_device_id* device, bool resume, bf_session_info* out,
              bf_error* error)
{
    return guarded(error, false, [&]() -> bool {
        if (!requireArguments(ctx != nullptr && device != nullptr, error, "ctx and device") ||
            !requireTransport(ctx, error)) {
            return false;
        }
        BusyGuard guard(ctx);
        if (!beginOperation(ctx, guard, error)) {
            return false;
        }
        auto interface = claim(ctx, *device);
        if (!interface) {
            setError(error, interface.error());
            return false;
        }
        auto info = flash::probe(**interface, {}, resume);
        if (!info) {
            setError(error, info.error());
            return false;
        }
        if (out != nullptr) {
            *out = toC(*info);
        }
        return true;
    });
}

bool bf_flash(bf_ctx* ctx, const bf_device_id* device, const bf_packages* packages,
              const bf_flash_options* options, bf_flash_report* report, bf_error* error)
{
    return guarded(error, false, [&]() -> bool {
        if (!requireArguments(ctx != nullptr && device != nullptr && packages != nullptr &&
                                  options != nullptr,
                              error, "ctx, device, packages and options are required") ||
            !requireArguments(options->typed_model != nullptr &&
                                  options->typed_model[0] != '\0',
                              error, "typed_model is required") ||
            !requireTransport(ctx, error)) {
            return false;
        }
        BusyGuard guard(ctx);
        if (!beginOperation(ctx, guard, error)) {
            return false;
        }
        flash::Request request;
        request.packages = packages->packages;
        request.confirmation = {options->typed_model, options->accept_data_erase};
        if (options->reviewed_plan != nullptr) {
            if (!options->reviewed_plan->plan) {
                setError(error, BF_E_INVALID_ARGUMENT,
                         "reviewed_plan must come from bf_plan_offline");
                return false;
            }
            request.reviewed = *options->reviewed_plan->plan;
        }
        flash::Options flashOptions;
        flashOptions.reboot = options->reboot;

        auto interface = claim(ctx, *device);
        if (!interface) {
            setError(error, interface.error());
            return false;
        }
        auto result =
            flash::flash(**interface, request, flashOptions, observerFor(ctx), &ctx->cancel);
        if (!result) {
            setError(error, result.error());
            return false;
        }
        if (report != nullptr) {
            *report = {toC(result->session), result->bytesTransferred, result->rebooted};
        }
        return true;
    });
}

} // extern "C"
