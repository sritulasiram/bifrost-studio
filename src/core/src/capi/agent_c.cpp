// C API over service::AgentClient: the app's connection to bifrost-agent.
#include "capi/agent_internal.hpp"
#include "capi/capi_types.hpp"

#include "bifrost/service/agent_client.hpp"
#include "bifrost/service/listener.hpp"
#include "service_config.hpp"

#include <memory>
#include <string>
#include <vector>

using namespace bifrost;
using namespace bifrost::capi;

static_assert(static_cast<int>(service::JobKind::flash) == BF_JOB_FLASH);
static_assert(static_cast<int>(service::JobKind::probe) == BF_JOB_PROBE);
static_assert(static_cast<int>(service::JobKind::readPit) == BF_JOB_READ_PIT);

struct bf_agent {
    std::unique_ptr<service::AgentClient> client;
};

struct bf_job {
    service::JobSnapshot snapshot;
    std::optional<bf_plan> plan;
};

namespace {

void fillResult(const service::JobResult& result, bf_job_result* out)
{
    *out = {};
    out->ok = !result.error.has_value();
    if (result.error) {
        setError(&out->error, *result.error);
    } else {
        clearError(&out->error);
    }
    out->session = toC(result.session);
    out->bytes_transferred = result.bytesTransferred;
    out->rebooted = result.rebooted;
    out->pit = result.pit.empty() ? nullptr : result.pit.data();
    out->pit_size = result.pit.size();
}

bf_progress toC(const flash::Progress& p)
{
    return {static_cast<std::int32_t>(p.phase), p.slot.c_str(), p.entry.c_str(),
            p.fileDone, p.fileTotal, p.totalDone, p.total};
}

transport::UsbIdentity identityOf(const bf_device_id& id)
{
    return {id.vendor_id, id.product_id, id.bus, id.address};
}

service::AgentEventHandler adapt(bf_agent_event_fn fn, void* user)
{
    if (fn == nullptr) {
        return {};
    }
    return [fn, user](const service::AgentEvent& event) {
        bf_agent_event out{};
        out.job = event.job;
        std::optional<bf_plan> plan;
        bf_job_result result{};
        using Kind = service::AgentEvent::Kind;
        switch (event.kind) {
        case Kind::started: out.kind = BF_EVENT_STARTED; break;
        case Kind::progress:
            out.kind = BF_EVENT_PROGRESS;
            out.progress = toC(event.progress);
            break;
        case Kind::log:
            out.kind = BF_EVENT_LOG;
            out.log_level = static_cast<std::int32_t>(event.log.level);
            out.log_message = event.log.message.c_str();
            break;
        case Kind::plan:
            out.kind = BF_EVENT_PLAN;
            if (event.plan) {
                plan = bf_plan{*event.plan, std::nullopt};
                out.plan = &*plan;
            }
            break;
        case Kind::finished:
            out.kind = BF_EVENT_FINISHED;
            if (event.result) {
                fillResult(*event.result, &result);
                out.result = &result;
            }
            break;
        case Kind::devicesChanged: out.kind = BF_EVENT_DEVICES_CHANGED; break;
        case Kind::disconnected: out.kind = BF_EVENT_DISCONNECTED; break;
        }
        fn(user, &out);
    };
}

bf_agent* wrap(Expected<std::unique_ptr<service::AgentClient>> client, bf_error* error)
{
    if (!client) {
        setError(error, client.error());
        return nullptr;
    }
    clearError(error);
    return new bf_agent{std::move(*client)};
}

bool requireAgent(bf_agent* agent, bf_error* error)
{
    if (agent == nullptr || !agent->client) {
        setError(error, BF_E_INVALID_ARGUMENT, "Invalid argument: agent");
        return false;
    }
    return true;
}

bool startResult(Expected<std::uint64_t> started, std::uint64_t* job, bf_error* error)
{
    if (!started) {
        setError(error, started.error());
        return false;
    }
    if (job != nullptr) {
        *job = *started;
    }
    clearError(error);
    return true;
}

} // namespace

namespace bifrost::capi {

bf_agent* connectAgentEndpoint(service::Xpc endpoint, bf_agent_event_fn fn, void* user,
                               bf_error* error)
{
    return guarded(error, static_cast<bf_agent*>(nullptr), [&]() -> bf_agent* {
        return wrap(service::AgentClient::endpoint(std::move(endpoint), adapt(fn, user)), error);
    });
}

} // namespace bifrost::capi

extern "C" {

bf_agent* bf_agent_connect(bf_agent_event_fn fn, void* user, bf_error* error)
{
    return guarded(error, static_cast<bf_agent*>(nullptr), [&]() -> bf_agent* {
        const auto requirement = service::signingRequirement(
            {service::config::kAgentIdentifier}, service::config::kTeamId);
        return wrap(service::AgentClient::machService(service::config::kAgentService,
                                                      requirement, adapt(fn, user)),
                    error);
    });
}

void bf_agent_free(bf_agent* agent)
{
    try {
        delete agent; // AgentClient guarantees no callback after this returns
    } catch (...) {
    }
}

bool bf_agent_subscribe(bf_agent* agent, bf_error* error)
{
    return guarded(error, false, [&]() -> bool {
        if (!requireAgent(agent, error)) {
            return false;
        }
        auto subscribed = agent->client->subscribe();
        if (!subscribed) {
            setError(error, subscribed.error());
            return false;
        }
        clearError(error);
        return true;
    });
}

bf_device_list* bf_agent_devices(bf_agent* agent, bf_error* error)
{
    return guarded(error, static_cast<bf_device_list*>(nullptr), [&]() -> bf_device_list* {
        if (!requireAgent(agent, error)) {
            return nullptr;
        }
        auto devices = agent->client->devices();
        if (!devices) {
            setError(error, devices.error());
            return nullptr;
        }
        clearError(error);
        return new bf_device_list{std::move(*devices)};
    });
}

bool bf_agent_start_flash(bf_agent* agent, const bf_agent_flash_request* request,
                          uint64_t* job, bf_error* error)
{
    return guarded(error, false, [&]() -> bool {
        if (!requireAgent(agent, error)) {
            return false;
        }
        if (request == nullptr || request->packages == nullptr || request->package_count == 0 ||
            request->typed_model == nullptr) {
            setError(error, BF_E_INVALID_ARGUMENT, "Invalid argument: flash request");
            return false;
        }
        service::FlashJobRequest flash;
        flash.device = identityOf(request->device);
        for (size_t i = 0; i < request->package_count; ++i) {
            const auto& p = request->packages[i];
            if (p.slot < BF_SLOT_BL || p.slot > BF_SLOT_USERDATA || p.path == nullptr) {
                setError(error, BF_E_INVALID_ARGUMENT, "Invalid argument: package slot or path");
                return false;
            }
            flash.packages.push_back({static_cast<planner::Slot>(p.slot), p.path});
        }
        flash.typedModel = request->typed_model;
        flash.acceptDataErase = request->accept_data_erase;
        flash.reboot = request->reboot;
        if (request->reviewed_pit != nullptr && request->reviewed_pit_size > 0) {
            flash.reviewedPit.assign(request->reviewed_pit,
                                     request->reviewed_pit + request->reviewed_pit_size);
        }
        return startResult(agent->client->startFlash(flash), job, error);
    });
}

bool bf_agent_start_probe(bf_agent* agent, const bf_device_id* device, bool resume,
                          uint64_t* job, bf_error* error)
{
    return guarded(error, false, [&]() -> bool {
        if (!requireAgent(agent, error) || device == nullptr) {
            setError(error, BF_E_INVALID_ARGUMENT, "Invalid argument: device");
            return false;
        }
        return startResult(agent->client->startProbe(identityOf(*device), resume), job, error);
    });
}

bool bf_agent_start_read_pit(bf_agent* agent, const bf_device_id* device, bool resume,
                             uint64_t* job, bf_error* error)
{
    return guarded(error, false, [&]() -> bool {
        if (!requireAgent(agent, error) || device == nullptr) {
            setError(error, BF_E_INVALID_ARGUMENT, "Invalid argument: device");
            return false;
        }
        return startResult(agent->client->startReadPit(identityOf(*device), resume), job,
                           error);
    });
}

bool bf_agent_cancel(bf_agent* agent, int32_t level, bf_error* error)
{
    return guarded(error, false, [&]() -> bool {
        if (!requireAgent(agent, error)) {
            return false;
        }
        if (level != BF_CANCEL_BEFORE_DATA && level != BF_CANCEL_AFTER_SEQUENCE) {
            setError(error, BF_E_INVALID_ARGUMENT, "Invalid argument: cancel level");
            return false;
        }
        auto cancelled = agent->client->cancel(static_cast<CancelLevel>(level));
        if (!cancelled) {
            setError(error, cancelled.error());
            return false;
        }
        clearError(error);
        return true;
    });
}

bf_job* bf_agent_status(bf_agent* agent, bf_error* error)
{
    return guarded(error, static_cast<bf_job*>(nullptr), [&]() -> bf_job* {
        if (!requireAgent(agent, error)) {
            return nullptr;
        }
        auto status = agent->client->status();
        if (!status) {
            setError(error, status.error());
            return nullptr;
        }
        clearError(error);
        if (!status->has_value()) {
            return nullptr;
        }
        auto* job = new bf_job{std::move(**status), std::nullopt};
        if (job->snapshot.plan) {
            job->plan = bf_plan{*job->snapshot.plan, std::nullopt};
        }
        return job;
    });
}

uint64_t bf_job_id(const bf_job* job) { return job == nullptr ? 0 : job->snapshot.id; }

int32_t bf_job_kind_of(const bf_job* job)
{
    return job == nullptr ? -1 : static_cast<int32_t>(job->snapshot.kind);
}

bool bf_job_running(const bf_job* job) { return job != nullptr && job->snapshot.running; }

bool bf_job_device(const bf_job* job, bf_device_id* out)
{
    if (job == nullptr || out == nullptr) {
        return false;
    }
    const auto& d = job->snapshot.device;
    *out = {d.vendorId, d.productId, d.busNumber, d.deviceAddress};
    return true;
}

void bf_job_progress(const bf_job* job, bf_progress* out)
{
    if (out == nullptr) {
        return;
    }
    if (job == nullptr) {
        *out = {BF_PHASE_VERIFYING, "", "", 0, 0, 0, 0};
        return;
    }
    *out = toC(job->snapshot.progress);
}

const bf_plan* bf_job_plan(const bf_job* job)
{
    return job != nullptr && job->plan ? &*job->plan : nullptr;
}

size_t bf_job_log_count(const bf_job* job) { return job == nullptr ? 0 : job->snapshot.log.size(); }

const char* bf_job_log(const bf_job* job, size_t index, int32_t* level)
{
    if (job == nullptr || index >= job->snapshot.log.size()) {
        return nullptr;
    }
    const auto& line = job->snapshot.log[index];
    if (level != nullptr) {
        *level = static_cast<int32_t>(line.level);
    }
    return line.message.c_str();
}

bool bf_job_result_get(const bf_job* job, bf_job_result* out)
{
    if (job == nullptr || out == nullptr || !job->snapshot.result) {
        return false;
    }
    fillResult(*job->snapshot.result, out);
    return true;
}

void bf_job_free(bf_job* job) { delete job; }

} // extern "C"
