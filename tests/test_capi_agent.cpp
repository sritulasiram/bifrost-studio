// The agent part of the C API (bf_agent_*), end to end over in-process XPC:
// C API -> AgentClient -> Agent -> RemoteTransport -> UsbService -> simulator.
#include "bifrost/bifrost.h"
#include "bifrost/service/agent.hpp"
#include "bifrost/service/listener.hpp"
#include "bifrost/service/remote_transport.hpp"
#include "bifrost/service/usb_service.hpp"
#include "capi/agent_internal.hpp"
#include "support/sim_device.hpp"
#include "support/test_support.hpp"

#include <lz4frame.h>

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <string>
#include <vector>

using namespace bifrost;
using namespace bifrost::service;
using test::Bytes;

namespace {

const std::vector<test::PitSpec> kLayout{{1, "BOOT", "boot.img"}, {2, "SYSTEM", "system.img"}};

struct Recorder {
    std::mutex mutex;
    std::condition_variable changed;
    std::vector<int32_t> kinds;
    std::size_t plans{};
    std::size_t planFiles{};
    bool finished{};
    bool ok{};
    int32_t code{};
    int32_t dataState{};
    std::vector<std::uint8_t> pit;
    std::uint64_t bytes{};
    std::uint16_t protocol{};

    void reset()
    {
        std::lock_guard<std::mutex> lock(mutex);
        kinds.clear();
        finished = false;
        protocol = 0;
        pit.clear();
    }
    bool waitFinished()
    {
        std::unique_lock<std::mutex> lock(mutex);
        return changed.wait_for(lock, std::chrono::seconds(20), [this] { return finished; });
    }
};

void onEvent(void* user, const bf_agent_event* event)
{
    auto* r = static_cast<Recorder*>(user);
    std::lock_guard<std::mutex> lock(r->mutex);
    r->kinds.push_back(event->kind);
    if (event->kind == BF_EVENT_PLAN && event->plan != nullptr) {
        ++r->plans;
        r->planFiles = bf_plan_file_count(event->plan);
    }
    if (event->kind == BF_EVENT_FINISHED && event->result != nullptr) {
        r->finished = true;
        r->ok = event->result->ok;
        r->code = event->result->error.code;
        r->dataState = event->result->error.data_state;
        r->bytes = event->result->bytes_transferred;
        r->protocol = event->result->session.protocol_version;
        if (event->result->pit != nullptr) {
            r->pit.assign(event->result->pit, event->result->pit + event->result->pit_size);
        }
    }
    r->changed.notify_all();
}

bool saw(Recorder& r, int32_t kind)
{
    std::lock_guard<std::mutex> lock(r.mutex);
    return std::find(r.kinds.begin(), r.kinds.end(), kind) != r.kinds.end();
}

} // namespace

int main()
{
    test::TempDir dir;
    Bytes boot(5000, 7);
    Bytes system(1500000);
    for (std::size_t i = 0; i < system.size(); ++i) system[i] = static_cast<std::uint8_t>(i >> 3);
    LZ4F_preferences_t prefs{};
    Bytes compressed(LZ4F_compressFrameBound(system.size(), &prefs));
    compressed.resize(LZ4F_compressFrame(compressed.data(), compressed.size(), system.data(),
                                         system.size(), &prefs));
    const auto tar = test::TarBuilder().file("boot.img", boot).file("system.img.lz4", compressed).finish();
    const std::string name = "AP_N950FXXU8DUH1_CL1.tar.md5";
    const auto path = dir.write(name, test::withMd5Trailer(tar, name));
    const auto pitBytes = test::makePit(kLayout);

    sim::Config config;
    config.pit = pitBytes;
    auto device = std::make_shared<sim::Device>(config);
    auto simTransport = std::make_shared<sim::Transport>(device);
    auto usbd = std::make_shared<UsbService>(simTransport);
    auto usbdListener =
        Listener::anonymous([usbd](xpc_connection_t peer) { usbd->accept(peer); }).value();
    auto agent = std::make_shared<Agent>(
        std::shared_ptr<transport::UsbTransport>(RemoteTransport::endpoint(usbdListener->endpoint())));
    auto agentListener =
        Listener::anonymous([agent](xpc_connection_t peer) { agent->accept(peer); }).value();

    Recorder recorder;
    bf_error error;
    bf_agent* client = capi::connectAgentEndpoint(agentListener->endpoint(), onEvent, &recorder, &error);
    CHECK(client != nullptr && error.code == BF_OK);
    if (client == nullptr) {
        return test::result();
    }
    CHECK(bf_agent_subscribe(client, &error));

    // No job yet.
    CHECK(bf_agent_status(client, &error) == nullptr && error.code == BF_OK);

    auto* list = bf_agent_devices(client, &error);
    bf_device_info info{};
    CHECK(list != nullptr && bf_device_list_count(list) == 1 && bf_device_list_get(list, 0, &info));
    CHECK(info.has_odin_interface && info.id.product_id == 0x685D);
    bf_device_list_free(list);

    // The app's flow: review against a PIT without a device session (the
    // CSC's embedded PIT), then flash as the first session of this entry.
    std::uint64_t job = 0;
    const bf_slot_path slots[] = {{BF_SLOT_AP, path.c_str()}};
    bf_agent_flash_request request{};
    request.device = info.id;
    request.packages = slots;
    request.package_count = 1;
    request.typed_model = "SM-N950F";
    request.reboot = false; // stay in Download Mode for the next job
    request.reviewed_pit = pitBytes.data();
    request.reviewed_pit_size = pitBytes.size();
    CHECK(bf_agent_start_flash(client, &request, &job, &error));
    CHECK(recorder.waitFinished() && recorder.ok &&
          recorder.bytes == boot.size() + system.size());
    CHECK(saw(recorder, BF_EVENT_STARTED) && saw(recorder, BF_EVENT_PLAN) &&
          saw(recorder, BF_EVENT_PROGRESS) && saw(recorder, BF_EVENT_LOG));
    CHECK(recorder.planFiles == 2);
    CHECK(device->written(1) && *device->written(1) == boot);
    CHECK(device->written(2) && *device->written(2) == system);
    CHECK(device->violations().empty() && device->repeatedHandshakes() == 0);
    CHECK(!device->rebooted());

    // Status snapshot after the job.
    bf_job* status = bf_agent_status(client, &error);
    CHECK(status != nullptr && !bf_job_running(status) && bf_job_kind_of(status) == BF_JOB_FLASH);
    if (status != nullptr) {
        const bf_plan* plan = bf_job_plan(status);
        CHECK(plan != nullptr && bf_plan_flashable(plan) && bf_plan_file_count(plan) == 2);
        CHECK(std::string(bf_plan_detected_model(plan)) == "N950F");
        bf_job_result result{};
        CHECK(bf_job_result_get(status, &result) && result.ok);
        CHECK(bf_job_log_count(status) > 0);
        bf_device_id id{};
        CHECK(bf_job_device(status, &id) && id.product_id == 0x685D);
        bf_job_free(status);
    }

    // Read-only jobs may resume in the same entry.
    recorder.reset();
    CHECK(bf_agent_start_read_pit(client, &info.id, true, &job, &error) && job > 0);
    CHECK(recorder.waitFinished() && recorder.ok && recorder.pit == pitBytes);
    CHECK(device->repeatedHandshakes() == 0);

    // Errors keep their code and data state.
    device->reenterDownloadMode();
    recorder.reset();
    request.typed_model = "SM-G973F";
    CHECK(bf_agent_start_flash(client, &request, &job, &error));
    CHECK(recorder.waitFinished() && !recorder.ok && recorder.code == BF_E_MODEL_MISMATCH &&
          recorder.dataState == BF_DATA_NONE);
    // A failed job still reports the session it opened.
    CHECK(recorder.protocol == 2 && recorder.bytes == 0);
    CHECK(!bf_agent_cancel(client, BF_CANCEL_BEFORE_DATA, &error) &&
          error.code == BF_E_ILLEGAL_STATE);
    CHECK(!bf_agent_cancel(client, 7, &error) && error.code == BF_E_INVALID_ARGUMENT);

    bf_agent_free(client);
    agentListener.reset();
    agent.reset();
    usbdListener.reset();
    return test::result();
}
