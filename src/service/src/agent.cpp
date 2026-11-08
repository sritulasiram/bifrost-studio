#include "bifrost/service/agent.hpp"

#include "bifrost/firmware/file_source.hpp"
#include "bifrost/flash/flash.hpp"
#include "bifrost/pit/pit.hpp"
#include "bifrost/planner/plan.hpp"
#include "bifrost/service/wire.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <deque>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

namespace bifrost::service {
namespace {

xpc_connection_t asConnection(const Xpc& x)
{
    return static_cast<xpc_connection_t>(x.get());
}

std::string describe(const transport::UsbIdentity& id)
{
    char text[64];
    std::snprintf(text, sizeof text, "%04x:%04x bus %u address %u", id.vendorId, id.productId,
                  static_cast<unsigned>(id.busNumber), static_cast<unsigned>(id.deviceAddress));
    return text;
}

struct FlashRequest {
    bool resume{};
    std::vector<flash::SlotPath> packages;
    std::string typedModel;
    bool acceptDataErase{};
    bool reboot{true};
    std::vector<std::uint8_t> reviewedPit;
};

} // namespace

struct Agent::Impl : std::enable_shared_from_this<Agent::Impl> {
    struct Job {
        JobSnapshot snapshot;
        CancelToken cancel;
        std::chrono::steady_clock::time_point lastProgressSent{};
        flash::Phase lastPhase{flash::Phase::verifying};
        std::string lastEntry;
    };

    std::shared_ptr<transport::UsbTransport> transport;
    AgentOptions options;

    mutable std::mutex mutex;
    std::vector<Xpc> peers;
    std::vector<Xpc> subscribers;
    std::shared_ptr<Job> job;
    std::thread worker;
    std::uint64_t nextJobId{1};
    bool hotplugTried{};
    transport::UsbEnumeration lastEnumeration;
    bool shuttingDown{};

    void handle(xpc_object_t message);
    void drop(xpc_connection_t peer);
    void broadcast(const Xpc& event);
    Xpc makeEvent(const char* name, std::uint64_t jobId);
    void appendLog(const std::shared_ptr<Job>& j, flash::LogLevel level, std::string message);
    void run(std::shared_ptr<Job> j, FlashRequest request);
    Xpc start(xpc_object_t message);
    void send(xpc_object_t request, const Xpc& reply)
    {
        if (reply) {
            xpc_connection_send_message(xpc_dictionary_get_remote_connection(request), reply.get());
        }
    }
};

Xpc Agent::Impl::makeEvent(const char* name, std::uint64_t jobId)
{
    auto event = Xpc::adopt(xpc_dictionary_create(nullptr, nullptr, 0));
    xpc_dictionary_set_string(event.get(), "event", name);
    if (jobId != 0) {
        xpc_dictionary_set_uint64(event.get(), "job", jobId);
    }
    return event;
}

void Agent::Impl::broadcast(const Xpc& event)
{
    std::vector<Xpc> targets;
    {
        std::lock_guard<std::mutex> lock(mutex);
        targets = subscribers;
    }
    for (const auto& target : targets) {
        xpc_connection_send_message(asConnection(target), event.get());
    }
}

void Agent::Impl::appendLog(const std::shared_ptr<Job>& j, flash::LogLevel level,
                            std::string message)
{
    {
        std::lock_guard<std::mutex> lock(mutex);
        auto& log = j->snapshot.log;
        log.push_back({level, message});
        if (log.size() > options.logLimit) {
            log.erase(log.begin(), log.begin() + static_cast<std::ptrdiff_t>(log.size() - options.logLimit));
        }
    }
    auto event = makeEvent("log", j->snapshot.id);
    xpc_dictionary_set_uint64(event.get(), "level", static_cast<std::uint64_t>(level));
    xpc_dictionary_set_string(event.get(), "message", message.c_str());
    broadcast(event);
}

void Agent::Impl::run(std::shared_ptr<Job> j, FlashRequest request)
{
    const auto id = j->snapshot.id;
    const auto kind = j->snapshot.kind;
    const auto device = j->snapshot.device;

    // Kept for the result even when the job fails, so a failure report says how
    // far it got (it used to show session 0 and 0 bytes after a 4.9 GB write).
    // The observer runs synchronously inside this function.
    std::optional<odin::SessionInfo> openedSession;
    std::uint64_t acknowledged = 0;

    flash::Observer observer;
    observer.onLog = [this, j](flash::LogLevel level, const std::string& message) {
        appendLog(j, level, message);
    };
    observer.onSession = [&openedSession](const odin::SessionInfo& info) {
        openedSession = info;
    };
    observer.onProgress = [this, j, id, &acknowledged](const flash::Progress& progress) {
        switch (progress.phase) {
        case flash::Phase::transferring:
        case flash::Phase::committing:
        case flash::Phase::closing:
        case flash::Phase::done:
            acknowledged = std::max(acknowledged, progress.totalDone);
            break;
        default:
            break;
        }
        bool sendNow = false;
        {
            std::lock_guard<std::mutex> lock(mutex);
            j->snapshot.progress = progress;
            const auto now = std::chrono::steady_clock::now();
            if (progress.phase != j->lastPhase || progress.entry != j->lastEntry ||
                progress.fileDone == progress.fileTotal ||
                now - j->lastProgressSent >= std::chrono::milliseconds(options.progressIntervalMs)) {
                sendNow = true;
                j->lastProgressSent = now;
                j->lastPhase = progress.phase;
                j->lastEntry = progress.entry;
            }
        }
        if (sendNow) {
            auto event = makeEvent("progress", id);
            xpc_dictionary_set_value(event.get(), "progress", encodeProgress(progress).get());
            broadcast(event);
        }
    };
    observer.onPlan = [this, j, id](const planner::FlashPlan& plan) {
        auto summary = planner::summarize(plan);
        {
            std::lock_guard<std::mutex> lock(mutex);
            j->snapshot.plan = summary;
        }
        auto event = makeEvent("plan", id);
        xpc_dictionary_set_value(event.get(), "plan", encodePlan(summary).get());
        broadcast(event);
    };

    appendLog(j, flash::LogLevel::info,
              std::string("Started ") + toString(kind) + " on " + describe(device));

    JobResult result;
    const auto failWith = [&](Error error) { result.error = std::move(error); };

    if (kind == JobKind::flash) {
        auto packages = flash::loadPackages(request.packages, observer, &j->cancel);
        if (!packages) {
            failWith(std::move(packages).error());
        } else {
            flash::Request flashRequest;
            flashRequest.packages = std::move(*packages);
            flashRequest.confirmation = {request.typedModel, request.acceptDataErase};
            bool ok = true;
            if (!request.reviewedPit.empty()) {
                auto table = pit::parse(request.reviewedPit.data(), request.reviewedPit.size());
                if (!table) {
                    failWith(std::move(table).error());
                    ok = false;
                } else {
                    flashRequest.reviewed = planner::buildPlan(*table, flashRequest.packages);
                }
            }
            if (ok) {
                auto interface = transport->claim(device);
                if (!interface) {
                    failWith(std::move(interface).error());
                } else {
                    flash::Options flashOptions;
                    flashOptions.reboot = request.reboot;
                    flashOptions.timeouts = options.timeouts;
                    auto report = flash::flash(**interface, flashRequest, flashOptions, observer,
                                               &j->cancel);
                    if (!report) {
                        failWith(std::move(report).error());
                        if (openedSession) {
                            result.session = *openedSession;
                        }
                        result.bytesTransferred = acknowledged;
                    } else {
                        result.session = report->session;
                        result.bytesTransferred = report->bytesTransferred;
                        result.rebooted = report->rebooted;
                    }
                }
            }
        }
    } else {
        auto interface = transport->claim(device);
        if (!interface) {
            failWith(std::move(interface).error());
        } else if (kind == JobKind::probe) {
            auto info = flash::probe(**interface, options.timeouts, request.resume);
            if (!info) {
                failWith(std::move(info).error());
            } else {
                result.session = *info;
            }
        } else {
            // Keep the raw bytes even when validation fails, for diagnosis.
            auto bytes = flash::readDevicePitBytes(**interface, options.timeouts, request.resume);
            if (!bytes) {
                failWith(std::move(bytes).error());
            } else {
                result.pit = std::move(*bytes);
                if (auto table = pit::parse(result.pit.data(), result.pit.size()); !table) {
                    failWith(std::move(table).error());
                }
            }
        }
    }

    if (result.error) {
        appendLog(j, flash::LogLevel::error,
                  std::string("Failed (") + toString(result.error->code) + ", " +
                      toString(result.error->dataState) + "): " + result.error->message);
    } else {
        appendLog(j, flash::LogLevel::info, std::string("Finished ") + toString(kind));
    }
    {
        std::lock_guard<std::mutex> lock(mutex);
        j->snapshot.result = result;
        j->snapshot.running = false;
    }
    auto event = makeEvent("finished", id);
    xpc_dictionary_set_value(event.get(), "result", encodeResult(result).get());
    broadcast(event);
}

Xpc Agent::Impl::start(xpc_object_t message)
{
    const auto kind = parseJobKind(getString(message, "kind"));
    const auto device = getIdentity(message);
    if (!kind || !device) {
        return replyError(message, makeError(ErrorCode::invalidArgument,
                                             "start needs a valid kind and device"));
    }
    if (device->vendorId != transport::kSamsungVendorId) {
        return replyError(message,
                          makeError(ErrorCode::invalidArgument, "Not a Samsung USB device"));
    }

    FlashRequest request;
    request.resume = getBool(message, "resume");
    if (*kind == JobKind::flash) {
        if (request.resume) {
            // An older client: flashes now always start with a fresh handshake.
            return replyError(message,
                              makeError(ErrorCode::invalidArgument,
                                        "A flash must be the first session after entering "
                                        "Download Mode; it cannot resume"));
        }
        request.typedModel = getString(message, "model");
        request.acceptDataErase = getBool(message, "acceptErase");
        request.reboot = getBool(message, "reboot");
        request.reviewedPit = getData(message, "reviewedPit");
        if (request.typedModel.empty()) {
            return replyError(message, makeError(ErrorCode::invalidArgument,
                                                 "A typed model is required to flash"));
        }
        xpc_object_t packages = xpc_dictionary_get_value(message, "packages");
        if (packages == nullptr || xpc_get_type(packages) != XPC_TYPE_ARRAY ||
            xpc_array_get_count(packages) == 0) {
            return replyError(message,
                              makeError(ErrorCode::invalidArgument, "No firmware packages"));
        }
        for (std::size_t i = 0; i < xpc_array_get_count(packages); ++i) {
            xpc_object_t item = xpc_array_get_value(packages, i);
            const auto slot = getUint(item, "slot");
            const auto name = getString(item, "name");
            if (!slot || *slot > static_cast<std::uint64_t>(planner::Slot::userdata) ||
                name.empty() || name.find('/') != std::string::npos) {
                return replyError(message, makeError(ErrorCode::invalidArgument,
                                                     "Each package needs a slot and a file name"));
            }
            const int fd = xpc_dictionary_dup_fd(item, "fd");
            if (fd < 0) {
                return replyError(message, makeError(ErrorCode::invalidArgument,
                                                     "Package " + name + " has no file descriptor"));
            }
            auto source = firmware::FileSource::adopt(fd, name);
            if (!source) {
                return replyError(message, source.error());
            }
            request.packages.push_back(
                {static_cast<planner::Slot>(*slot), name, std::move(*source)});
        }
    }

    // Reserve the job slot under the lock, but join the previous (finished)
    // worker outside it: that thread's final broadcast also takes the lock.
    std::shared_ptr<Job> j;
    std::thread previous;
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (shuttingDown) {
            return replyError(message, makeError(ErrorCode::serviceUnavailable,
                                                 "The agent is shutting down"));
        }
        if (job && job->snapshot.running) {
            return replyError(message, makeError(ErrorCode::illegalState,
                                                 "Another job is already running"));
        }
        previous = std::move(worker);
        j = std::make_shared<Job>();
        j->snapshot.id = nextJobId++;
        j->snapshot.kind = *kind;
        j->snapshot.running = true;
        j->snapshot.device = *device;
        job = j;
    }
    if (previous.joinable()) {
        previous.join();
    }
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (shuttingDown) {
            j->snapshot.running = false;
            j->snapshot.result =
                JobResult{makeError(ErrorCode::serviceUnavailable, "The agent is shutting down"),
                          {}, 0, false, {}};
            return replyError(message, *j->snapshot.result->error);
        }
        worker = std::thread([self = shared_from_this(), j, request = std::move(request)]() mutable {
            self->run(j, std::move(request));
        });
    }
    auto started = makeEvent("started", j->snapshot.id);
    xpc_dictionary_set_string(started.get(), "kind", toString(*kind));
    broadcast(started);

    auto reply = replyOk(message);
    if (reply) {
        xpc_dictionary_set_uint64(reply.get(), "job", j->snapshot.id);
    }
    return reply;
}

void Agent::Impl::handle(xpc_object_t message)
{
    if (xpc_get_type(message) != XPC_TYPE_DICTIONARY) {
        return;
    }
    const auto op = getString(message, "op");
    if (op == "hello") {
        auto reply = replyOk(message);
        if (reply) {
            xpc_dictionary_set_uint64(reply.get(), "version", kWireVersion);
            xpc_dictionary_set_string(reply.get(), "role", "agent");
        }
        return send(message, reply);
    }
    if (getUint(message, "version").value_or(0) != kWireVersion) {
        return send(message, replyError(message, makeError(ErrorCode::protocolViolation,
                                                           "Agent: wire version mismatch")));
    }
    if (op == "devices") {
        // While a job runs, never touch USB topology: answer from the last
        // list. Enumerating during re-enumeration crashed libusb (usbd).
        std::optional<transport::UsbEnumeration> cached;
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (job && job->snapshot.running) {
                cached = lastEnumeration;
            }
        }
        auto enumeration = cached ? Expected<transport::UsbEnumeration>(*cached)
                                  : transport->enumerate();
        if (!enumeration) {
            return send(message, replyError(message, enumeration.error()));
        }
        if (!cached) {
            std::lock_guard<std::mutex> lock(mutex);
            lastEnumeration = *enumeration;
        }
        auto reply = replyOk(message);
        if (reply) {
            xpc_dictionary_set_value(reply.get(), "enumeration",
                                     encodeEnumeration(*enumeration).get());
        }
        return send(message, reply);
    }
    if (op == "status") {
        std::optional<JobSnapshot> snapshot;
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (job) {
                snapshot = job->snapshot;
            }
        }
        auto reply = replyOk(message);
        if (reply && snapshot) {
            xpc_dictionary_set_value(reply.get(), "job", encodeSnapshot(*snapshot).get());
        }
        return send(message, reply);
    }
    if (op == "subscribe") {
        bool tryHotplug = false;
        {
            std::lock_guard<std::mutex> lock(mutex);
            const auto peer = xpc_dictionary_get_remote_connection(message);
            const bool known = std::any_of(subscribers.begin(), subscribers.end(),
                                           [&](const Xpc& s) { return s.get() == peer; });
            if (!known) {
                subscribers.push_back(Xpc::retain(peer));
            }
            tryHotplug = !hotplugTried;
            hotplugTried = true;
        }
        if (tryHotplug) {
            std::weak_ptr<Impl> weak = shared_from_this();
            transport->startHotplug([weak] {
                if (auto self = weak.lock()) {
                    self->broadcast(self->makeEvent("devices-changed", 0));
                }
            });
        }
        return send(message, replyOk(message));
    }
    if (op == "start") {
        return send(message, start(message));
    }
    if (op == "cancel") {
        const auto level = getUint(message, "level").value_or(0);
        if (level != static_cast<std::uint64_t>(CancelLevel::beforeData) &&
            level != static_cast<std::uint64_t>(CancelLevel::afterSequence)) {
            return send(message, replyError(message, makeError(ErrorCode::invalidArgument,
                                                               "Unknown cancel level")));
        }
        std::shared_ptr<Job> current;
        {
            std::lock_guard<std::mutex> lock(mutex);
            current = job;
        }
        if (!current || !current->snapshot.running) {
            return send(message, replyError(message, makeError(ErrorCode::illegalState,
                                                               "No job is running")));
        }
        current->cancel.request(static_cast<CancelLevel>(level));
        appendLog(current, flash::LogLevel::warning,
                  level == static_cast<std::uint64_t>(CancelLevel::afterSequence)
                      ? "Stop requested after the current sequence (device may be partially written)"
                      : "Cancel requested (honoured only before any data is sent)");
        return send(message, replyOk(message));
    }
    send(message, replyError(message, makeError(ErrorCode::invalidArgument,
                                                "Agent: unknown operation '" + op + "'")));
}

void Agent::Impl::drop(xpc_connection_t peer)
{
    std::lock_guard<std::mutex> lock(mutex);
    const auto matches = [&](const Xpc& x) { return x.get() == peer; };
    subscribers.erase(std::remove_if(subscribers.begin(), subscribers.end(), matches),
                      subscribers.end());
    peers.erase(std::remove_if(peers.begin(), peers.end(), matches), peers.end());
}

Agent::Agent(std::shared_ptr<transport::UsbTransport> transport, AgentOptions options)
    : impl_(std::make_shared<Impl>())
{
    impl_->transport = std::move(transport);
    impl_->options = options;
}

Agent::~Agent()
{
    shutdown();
    std::vector<Xpc> peers;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        peers = impl_->peers;
    }
    for (const auto& peer : peers) {
        xpc_connection_cancel(asConnection(peer));
    }
    impl_->transport->stopHotplug();
}

void Agent::accept(xpc_connection_t peer)
{
    std::shared_ptr<Impl> impl = impl_;
    {
        std::lock_guard<std::mutex> lock(impl->mutex);
        impl->peers.push_back(Xpc::retain(peer));
    }
    xpc_connection_set_event_handler(peer, ^(xpc_object_t event) {
        if (xpc_get_type(event) == XPC_TYPE_ERROR) {
            impl->drop(peer);
            return;
        }
        impl->handle(event);
    });
    xpc_connection_resume(peer);
}

bool Agent::jobRunning() const
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->job && impl_->job->snapshot.running;
}

void Agent::shutdown()
{
    std::thread worker;
    std::shared_ptr<Impl::Job> running;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        impl_->shuttingDown = true;
        if (impl_->job && impl_->job->snapshot.running) {
            impl_->job->cancel.request(CancelLevel::afterSequence);
            running = impl_->job;
        }
        worker = std::move(impl_->worker);
    }
    if (running) {
        impl_->appendLog(running, flash::LogLevel::warning,
                         "The agent is shutting down: stopping at the next safe point");
    }
    if (worker.joinable()) {
        worker.join();
    }
}

} // namespace bifrost::service
