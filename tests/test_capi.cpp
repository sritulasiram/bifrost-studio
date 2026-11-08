// The C API, driven end to end through the bootloader simulator.
#include "bifrost/bifrost.h"
#include "capi/capi_internal.hpp"
#include "support/sim_device.hpp"
#include "support/test_support.hpp"

#include <lz4frame.h>

#include <memory>
#include <string>
#include <vector>

using test::Bytes;
using test::TarBuilder;

namespace {

const std::vector<test::PitSpec> kLayout{
    {1, "BOOT", "boot.img"},
    {2, "SYSTEM", "system.img"},
};

Bytes lz4Compress(const Bytes& source)
{
    LZ4F_preferences_t preferences{};
    Bytes out(LZ4F_compressFrameBound(source.size(), &preferences));
    out.resize(LZ4F_compressFrame(out.data(), out.size(), source.data(), source.size(),
                                  &preferences));
    return out;
}

struct Sim {
    std::shared_ptr<sim::Device> device;
    sim::Transport* transport{};
    bf_ctx* ctx{};

    explicit Sim(sim::Faults faults = {})
    {
        sim::Config config;
        config.pit = test::makePit(kLayout);
        config.faults = faults;
        device = std::make_shared<sim::Device>(config);
        auto owned = std::make_unique<sim::Transport>(device);
        transport = owned.get();
        ctx = bifrost::capi::createContext(std::move(owned));
    }
    ~Sim() { bf_destroy(ctx); }
    Sim(const Sim&) = delete;
    Sim& operator=(const Sim&) = delete;
};

bf_device_id simId()
{
    const auto id = sim::Transport::identity();
    return {id.vendorId, id.productId, id.busNumber, id.deviceAddress};
}

struct Recorder {
    std::vector<std::int32_t> phases;
    std::vector<std::string> logs;
    std::size_t plans{};
    std::size_t plannedFiles{};
    std::size_t hotplugs{};
    bf_ctx* reentrantCtx{};
    std::int32_t reentrantCode{-1};
};

void onProgress(void* user, const bf_progress* p)
{
    auto* r = static_cast<Recorder*>(user);
    if (r->phases.empty() || r->phases.back() != p->phase) {
        r->phases.push_back(p->phase);
    }
    if (r->reentrantCtx != nullptr && r->reentrantCode == -1) {
        // A second long operation on the same context must be refused.
        bf_error error;
        const auto id = simId();
        bf_session_info info;
        bf_probe(r->reentrantCtx, &id, false, &info, &error);
        r->reentrantCode = error.code;
    }
}

void onLog(void* user, std::int32_t, const char* message)
{
    static_cast<Recorder*>(user)->logs.emplace_back(message);
}

void onPlan(void* user, const bf_plan* plan)
{
    auto* r = static_cast<Recorder*>(user);
    ++r->plans;
    r->plannedFiles = bf_plan_file_count(plan);
}

void onHotplug(void* user) { ++static_cast<Recorder*>(user)->hotplugs; }

bool contains(const std::vector<std::int32_t>& values, std::int32_t value)
{
    return std::find(values.begin(), values.end(), value) != values.end();
}

} // namespace

int main()
{
    CHECK(bf_api_version() == BF_API_VERSION);
    CHECK(std::string(bf_version()) == "1.0.0");
    CHECK(std::string(bf_code_name(BF_E_USB_STALL)) == bifrost::toString(bifrost::ErrorCode::usbStall));
    CHECK(std::string(bf_code_name(BF_E_UNSUPPORTED)) == "unsupported");
    CHECK(std::string(bf_code_name(9999)) == "unknown");
    CHECK(std::string(bf_data_state_name(BF_DATA_PARTIAL)) == "partial");

    // Null handles are harmless.
    bf_destroy(nullptr);
    CHECK(bf_device_list_count(nullptr) == 0 && bf_plan_file_count(nullptr) == 0);
    CHECK(!bf_plan_flashable(nullptr) && bf_pit_entry_count(nullptr) == 0);

    // Built without a USB transport (this test links default_transport_none).
    {
        bf_error error;
        bf_ctx* ctx = bf_create(&error);
        CHECK(ctx != nullptr && error.code == BF_OK);
        CHECK(bf_devices(ctx, &error) == nullptr && error.code == BF_E_UNSUPPORTED);
        CHECK(!bf_start_hotplug(ctx, onHotplug, nullptr));
        bf_destroy(ctx);
    }

    // Firmware on disk.
    test::TempDir dir;
    Bytes boot(700001);
    Bytes system(3 * 1024 * 1024 + 9);
    for (std::size_t i = 0; i < boot.size(); ++i) boot[i] = static_cast<std::uint8_t>(i * 13);
    for (std::size_t i = 0; i < system.size(); ++i) system[i] = static_cast<std::uint8_t>(i / 7);
    const auto tar = TarBuilder()
                         .file("boot.img", boot)
                         .file("system.img.lz4", lz4Compress(system))
                         .file("meta-data/fota.zip", Bytes(16, 3))
                         .finish();
    const std::string apName = "AP_N950FXXU8DUH1_CL1.tar.md5";
    const auto apPath = dir.write(apName, test::withMd5Trailer(tar, apName));
    const auto pitBytes = test::makePit(kLayout);
    const auto pitPath = dir.write("device.pit", pitBytes);
    const bf_slot_path slots[] = {{BF_SLOT_AP, apPath.c_str()}};

    // Devices and hot-plug.
    {
        Sim sim;
        Recorder recorder;
        bf_error error;
        CHECK(bf_start_hotplug(sim.ctx, onHotplug, &recorder));
        auto* list = bf_devices(sim.ctx, &error);
        CHECK(list != nullptr && bf_device_list_count(list) == 1);
        bf_device_info info;
        CHECK(bf_device_list_get(list, 0, &info) && info.has_odin_interface);
        CHECK(info.id.vendor_id == 0x04E8 && info.id.product_id == 0x685D);
        CHECK(info.endpoint_in == 0x81 && info.endpoint_out == 0x01);
        CHECK(!bf_device_list_get(list, 1, &info));
        bf_device_list_free(list);

        sim.transport->setAttached(false);
        CHECK(recorder.hotplugs == 1);
        list = bf_devices(sim.ctx, &error);
        CHECK(list != nullptr && bf_device_list_count(list) == 0);
        bf_device_list_free(list);
        const auto id = simId();
        bf_session_info session;
        CHECK(!bf_probe(sim.ctx, &id, false, &session, &error) &&
              error.code == BF_E_USB_DISCONNECTED && error.data_state == BF_DATA_NONE);

        bf_stop_hotplug(sim.ctx);
        sim.transport->setAttached(true);
        CHECK(recorder.hotplugs == 1);
    }

    // Packages, PIT and offline plan.
    {
        Sim sim;
        Recorder recorder;
        bf_set_progress_callback(sim.ctx, onProgress, &recorder);
        bf_set_log_callback(sim.ctx, onLog, &recorder);
        bf_error error;

        CHECK(bf_packages_load(sim.ctx, nullptr, 1, &error) == nullptr &&
              error.code == BF_E_INVALID_ARGUMENT);
        const bf_slot_path badSlot[] = {{7, apPath.c_str()}};
        CHECK(bf_packages_load(sim.ctx, badSlot, 1, &error) == nullptr &&
              error.code == BF_E_INVALID_ARGUMENT);
        const bf_slot_path missing[] = {{BF_SLOT_AP, "/nonexistent/AP_x.tar.md5"}};
        CHECK(bf_packages_load(sim.ctx, missing, 1, &error) == nullptr && error.code != BF_OK);

        auto* packages = bf_packages_load(sim.ctx, slots, 1, &error);
        CHECK(packages != nullptr && error.code == BF_OK);
        CHECK(contains(recorder.phases, BF_PHASE_VERIFYING));
        CHECK(!recorder.logs.empty());
        bf_package_info info;
        CHECK(bf_packages_get(packages, 0, &info));
        CHECK(info.slot == BF_SLOT_AP && info.kind == BF_KIND_AP && info.checksum_valid);
        CHECK(std::string(info.model) == "N950F" && info.entry_count == 3);
        CHECK(std::string(info.build) == "N950FXXU8DUH1");
        bf_entry_info entry;
        CHECK(bf_packages_entry(packages, 0, 1, &entry) && entry.lz4 &&
              std::string(entry.name) == "system.img.lz4");
        CHECK(!bf_packages_entry(packages, 0, 3, &entry));

        auto* pit = bf_pit_load_file(pitPath.c_str(), &error);
        CHECK(pit != nullptr && bf_pit_entry_count(pit) == 2);
        bf_pit_entry pitEntry;
        CHECK(bf_pit_entry_get(pit, 1, &pitEntry) && pitEntry.identifier == 2 &&
              std::string(pitEntry.partition_name) == "SYSTEM" &&
              std::string(pitEntry.flash_filename) == "system.img");
        std::size_t size = 0;
        CHECK(bf_pit_bytes(pit, &size) != nullptr && size == pitBytes.size());

        CHECK(bf_pit_parse(pitBytes.data(), 10, &error) == nullptr &&
              error.code == BF_E_PIT_INVALID);
        CHECK(bf_pit_load_file(dir.write("big.pit", Bytes(2 * 1024 * 1024)).c_str(),
                               &error) == nullptr &&
              error.code == BF_E_PIT_INVALID);

        auto* plan = bf_plan_offline(pit, packages, &error);
        CHECK(plan != nullptr && bf_plan_flashable(plan));
        CHECK(bf_plan_file_count(plan) == 2 && bf_plan_skipped_count(plan) == 1);
        CHECK(bf_plan_refusal_count(plan) == 0 && bf_plan_refusal(plan, 0) == nullptr);
        CHECK(std::string(bf_plan_detected_model(plan)) == "N950F");
        bf_planned_file file;
        CHECK(bf_plan_file(plan, 1, &file) && file.partition_id == 2 &&
              std::string(file.partition) == "SYSTEM" && file.lz4);
        bf_skipped_entry skipped;
        CHECK(bf_plan_skipped(plan, 0, &skipped) &&
              std::string(skipped.entry) == "meta-data/fota.zip");

        // A smaller device PIT: the offline plan carries a refusal.
        const auto smallPit = test::makePit({{1, "BOOT", "boot.img"}});
        auto* small = bf_pit_parse(smallPit.data(), smallPit.size(), &error);
        auto* refused = bf_plan_offline(small, packages, &error);
        CHECK(refused != nullptr && !bf_plan_flashable(refused) &&
              bf_plan_refusal_count(refused) > 0);
        bf_plan_free(refused);
        bf_pit_free(small);

        bf_plan_free(plan);
        bf_pit_free(pit);
        bf_packages_free(packages);
    }

    // Long UTF-8 message: truncated to the buffer without splitting a character.
    {
        bf_error error;
        std::string path = "/nonexistent/";
        while (path.size() < 3000) {
            path += "\xC3\xA9"; // é
        }
        CHECK(bf_pit_load_file(path.c_str(), &error) == nullptr && error.code == BF_E_IO);
        const std::string message(error.message);
        CHECK(message.size() < BF_ERROR_MESSAGE_SIZE && message.size() > 900);
        CHECK((static_cast<unsigned char>(message.back()) & 0xC0) == 0x80);   // continuation
        CHECK((static_cast<unsigned char>(message[message.size() - 2]) & 0xE0) == 0xC0); // lead
    }

    // Probe and PIT read.
    {
        Sim sim;
        bf_error error;
        const auto id = simId();
        bf_session_info session;
        CHECK(bf_probe(sim.ctx, &id, false, &session, &error));
        CHECK(session.protocol_version == 2 && session.packet_size == 1048576);
        CHECK(sim.device->sessionEnded());
    }
    {
        Sim sim;
        bf_error error;
        const auto id = simId();
        auto* pit = bf_read_pit(sim.ctx, &id, false, &error);
        std::size_t size = 0;
        const auto* bytes = bf_pit_bytes(pit, &size);
        CHECK(pit != nullptr && size == pitBytes.size() &&
              std::equal(bytes, bytes + size, pitBytes.begin()));
        bf_pit_free(pit);
        const bf_device_id notSamsung{0x1234, 1, 1, 5};
        CHECK(bf_read_pit(sim.ctx, &notSamsung, false, &error) == nullptr &&
              error.code == BF_E_INVALID_ARGUMENT);
    }

    // Successful flash with a reviewed plan, callbacks and re-entrancy guard.
    {
        Sim sim;
        Recorder recorder;
        recorder.reentrantCtx = sim.ctx;
        bf_set_progress_callback(sim.ctx, onProgress, &recorder);
        bf_set_plan_callback(sim.ctx, onPlan, &recorder);
        bf_error error;
        auto* packages = bf_packages_load(sim.ctx, slots, 1, &error);
        auto* pit = bf_pit_parse(pitBytes.data(), pitBytes.size(), &error);
        auto* reviewed = bf_plan_offline(pit, packages, &error);
        recorder.phases.clear();
        recorder.reentrantCode = -1;

        const auto id = simId();
        bf_flash_options options{"SM-N950F", false, true, reviewed};
        bf_flash_report report;
        const bool flashed = bf_flash(sim.ctx, &id, packages, &options, &report, &error);
        if (!flashed) {
            std::cerr << "flash failed: " << error.message << '\n';
        }
        CHECK(flashed && error.code == BF_OK);
        CHECK(report.bytes_transferred == boot.size() + system.size() && report.rebooted);
        CHECK(recorder.plans == 1 && recorder.plannedFiles == 2);
        CHECK(contains(recorder.phases, BF_PHASE_COMMITTING));
        CHECK(!recorder.phases.empty() && recorder.phases.back() == BF_PHASE_DONE);
        CHECK(recorder.reentrantCode == BF_E_ILLEGAL_STATE);
        CHECK(sim.device->written(1) && *sim.device->written(1) == boot);
        CHECK(sim.device->written(2) && *sim.device->written(2) == system);
        CHECK(sim.device->violations().empty());

        bf_plan_free(reviewed);
        bf_pit_free(pit);
        bf_packages_free(packages);
    }

    // Refusals and failures carry the right code and data state.
    {
        Sim sim;
        bf_error error;
        auto* packages = bf_packages_load(sim.ctx, slots, 1, &error);
        const auto id = simId();
        bf_flash_options options{"SM-G973F", false, true, nullptr};
        CHECK(!bf_flash(sim.ctx, &id, packages, &options, nullptr, &error));
        CHECK(error.code == BF_E_MODEL_MISMATCH && error.data_state == BF_DATA_NONE);
        CHECK(sim.device->dataBytesReceived() == 0);

        options.typed_model = "";
        CHECK(!bf_flash(sim.ctx, &id, packages, &options, nullptr, &error) &&
              error.code == BF_E_INVALID_ARGUMENT);
        bf_packages_free(packages);
    }
    {
        sim::Faults faults;
        faults.stallAtPart = 1;
        Sim sim(faults);
        bf_error error;
        auto* packages = bf_packages_load(sim.ctx, slots, 1, &error);
        const auto id = simId();
        bf_flash_options options{"SM-N950F", false, true, nullptr};
        CHECK(!bf_flash(sim.ctx, &id, packages, &options, nullptr, &error));
        CHECK(error.code == BF_E_USB_STALL && error.data_state == BF_DATA_PARTIAL);
        CHECK(std::string(error.message).size() > 0);
        bf_packages_free(packages);
    }

    // A stale cancel request stops the next flash before data; reset clears it.
    {
        Sim sim;
        bf_error error;
        auto* packages = bf_packages_load(sim.ctx, slots, 1, &error);
        const auto id = simId();
        bf_flash_options options{"SM-N950F", false, false, nullptr};
        bf_cancel(sim.ctx, BF_CANCEL_BEFORE_DATA);
        CHECK(!bf_flash(sim.ctx, &id, packages, &options, nullptr, &error));
        CHECK(error.code == BF_E_CANCELLED && error.data_state == BF_DATA_NONE);
        CHECK(sim.device->dataBytesReceived() == 0);
        bf_packages_free(packages);
    }
    {
        Sim sim;
        bf_error error;
        auto* packages = bf_packages_load(sim.ctx, slots, 1, &error);
        const auto id = simId();
        bf_flash_options options{"SM-N950F", false, false, nullptr};
        bf_cancel(sim.ctx, BF_CANCEL_BEFORE_DATA);
        bf_cancel_reset(sim.ctx);
        CHECK(bf_flash(sim.ctx, &id, packages, &options, nullptr, &error));
        CHECK(!sim.device->rebooted());
        bf_packages_free(packages);
    }

    return test::result();
}
