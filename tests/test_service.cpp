// Background agent and USB helper over real XPC (anonymous endpoints, in
// process): client -> agent -> RemoteTransport -> UsbService -> simulator.
#include "bifrost/service/agent.hpp"
#include "bifrost/service/agent_client.hpp"
#include "bifrost/service/listener.hpp"
#include "bifrost/service/remote_transport.hpp"
#include "bifrost/service/usb_service.hpp"
#include "support/sim_device.hpp"
#include "support/test_support.hpp"

#include <lz4frame.h>

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <functional>
#include <mutex>
#include <thread>

using namespace bifrost;
using namespace bifrost::service;
using planner::Slot;
using test::Bytes;
using test::TarBuilder;

namespace {

constexpr auto kWait = std::chrono::seconds(20);

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

Bytes pattern(std::size_t size, unsigned seed)
{
    Bytes bytes(size);
    for (std::size_t i = 0; i < size; ++i) {
        bytes[i] = static_cast<std::uint8_t>((i * (seed + 3)) ^ (i >> 11));
    }
    return bytes;
}

struct Firmware {
    Bytes boot;
    Bytes system;
    std::string path;
};

Firmware makeFirmware(const test::TempDir& dir, const std::string& tag, std::size_t systemSize)
{
    Firmware f{pattern(700001, 1), pattern(systemSize, 2), {}};
    const auto tar = TarBuilder()
                         .file("boot.img", f.boot)
                         .file("system.img.lz4", lz4Compress(f.system))
                         .finish();
    const auto name = "AP_N950FXXU8DUH1_" + tag + ".tar.md5";
    f.path = dir.write(name, test::withMd5Trailer(tar, name));
    return f;
}

// Holds a transfer at one data part until released.
class Gate {
public:
    explicit Gate(std::uint64_t part) : part_(part) {}
    std::function<void(std::uint64_t)> hook()
    {
        return [this](std::uint64_t part) {
            if (part != part_) {
                return;
            }
            std::unique_lock<std::mutex> lock(mutex_);
            reached_ = true;
            changed_.notify_all();
            changed_.wait(lock, [this] { return open_; });
        };
    }
    bool waitReached()
    {
        std::unique_lock<std::mutex> lock(mutex_);
        return changed_.wait_for(lock, kWait, [this] { return reached_; });
    }
    void open()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        open_ = true;
        changed_.notify_all();
    }

private:
    std::uint64_t part_;
    std::mutex mutex_;
    std::condition_variable changed_;
    bool reached_{};
    bool open_{};
};

// Thread-safe event recorder.
class Events {
public:
    AgentEventHandler handler()
    {
        return [this](const AgentEvent& event) {
            std::lock_guard<std::mutex> lock(mutex_);
            events_.push_back(event);
            changed_.notify_all();
        };
    }
    std::optional<AgentEvent> waitFor(AgentEvent::Kind kind)
    {
        std::unique_lock<std::mutex> lock(mutex_);
        std::optional<AgentEvent> found;
        changed_.wait_for(lock, kWait, [&] {
            for (const auto& e : events_) {
                if (e.kind == kind) {
                    found = e;
                    return true;
                }
            }
            return false;
        });
        return found;
    }
    bool saw(AgentEvent::Kind kind, const std::function<bool(const AgentEvent&)>& match = {})
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return std::any_of(events_.begin(), events_.end(), [&](const AgentEvent& e) {
            return e.kind == kind && (!match || match(e));
        });
    }

private:
    std::mutex mutex_;
    std::condition_variable changed_;
    std::vector<AgentEvent> events_;
};

// The whole chain, wired with anonymous endpoints.
struct Chain {
    std::shared_ptr<sim::Device> device;
    std::shared_ptr<sim::Transport> simTransport;
    std::shared_ptr<UsbService> usbd;
    std::unique_ptr<Listener> usbdListener;
    std::shared_ptr<Agent> agent;
    std::unique_ptr<Listener> agentListener;

    explicit Chain(sim::Config config = baseConfig())
    {
        device = std::make_shared<sim::Device>(std::move(config));
        simTransport = std::make_shared<sim::Transport>(device);
        usbd = std::make_shared<UsbService>(simTransport);
        auto service = usbd;
        usbdListener =
            Listener::anonymous([service](xpc_connection_t peer) { service->accept(peer); })
                .value();
        std::shared_ptr<transport::UsbTransport> remote =
            RemoteTransport::endpoint(usbdListener->endpoint());
        agent = std::make_shared<Agent>(remote);
        auto host = agent;
        agentListener =
            Listener::anonymous([host](xpc_connection_t peer) { host->accept(peer); }).value();
    }
    ~Chain()
    {
        agentListener.reset();
        agent.reset();
        usbdListener.reset();
        usbd.reset();
    }

    std::unique_ptr<AgentClient> client(AgentEventHandler handler = {})
    {
        auto connected = AgentClient::endpoint(agentListener->endpoint(), std::move(handler));
        CHECK(connected);
        return connected ? std::move(*connected) : nullptr;
    }

    static sim::Config baseConfig()
    {
        sim::Config config;
        config.pit = test::makePit(kLayout);
        return config;
    }
};

FlashJobRequest flashRequest(const Firmware& firmware, const std::string& model = "SM-N950F")
{
    FlashJobRequest request;
    request.device = sim::Transport::identity();
    request.packages = {{Slot::ap, firmware.path}};
    request.typedModel = model;
    request.reboot = true;
    return request;
}

void checkWritten(const sim::Device& device, const Firmware& firmware)
{
    CHECK(device.written(1) && *device.written(1) == firmware.boot);
    CHECK(device.written(2) && *device.written(2) == firmware.system);
    for (const auto& v : device.violations()) {
        std::cerr << "simulator violation: " << v << '\n';
    }
    CHECK(device.violations().empty());
}

bool waitUntil(const std::function<bool()>& condition)
{
    const auto deadline = std::chrono::steady_clock::now() + kWait;
    while (std::chrono::steady_clock::now() < deadline) {
        if (condition()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return condition();
}

} // namespace

int main()
{
    test::TempDir dir;
    const auto small = makeFirmware(dir, "SMALL", 2 * 1024 * 1024 + 5);
    const auto large = makeFirmware(dir, "LARGE", 31 * 1024 * 1024 + 7);

    // Requirement strings.
    CHECK(signingRequirement({"a"}, "") == "identifier \"a\"");
    CHECK(signingRequirement({"a", "b"}, "TEAM123456") ==
          "anchor apple generic and certificate leaf[subject.OU] = \"TEAM123456\" and "
          "(identifier \"a\" or identifier \"b\")");

    // ---- USB helper ----------------------------------------------------------

    // Flash through the helper with RemoteTransport: byte-exact.
    {
        Chain chain;
        auto remote = RemoteTransport::endpoint(chain.usbdListener->endpoint());
        auto devices = remote->enumerate();
        CHECK(devices && devices->devices.size() == 1 && devices->devices[0].odinInterface);
        auto interface = remote->claim(sim::Transport::identity());
        CHECK(interface);
        if (interface) {
            CHECK((*interface)->selection().endpointIn == 0x81);
            auto packages = flash::loadPackages({{Slot::ap, small.path}});
            CHECK(packages);
            auto report = flash::flash(**interface, {*packages, {"SM-N950F", false}, std::nullopt});
            CHECK(report);
            checkWritten(*chain.device, small);
            CHECK(chain.device->rebooted());
        }
    }

    // Helper policy, with raw requests on one connection.
    {
        Chain chain;
        auto remote = RemoteTransport::endpoint(chain.usbdListener->endpoint());
        auto connection = remote->connect();
        CHECK(connection);
        const auto send = [&](const Xpc& message) {
            return call(static_cast<xpc_connection_t>(connection->get()), message.get(), "helper",
                        ErrorCode::serviceUnavailable, ErrorCode::serviceUnavailable);
        };
        const auto codeOf = [](const Expected<Xpc>& reply) {
            return reply ? ErrorCode{} : reply.error().code;
        };

        auto read = makeRequest("read");
        xpc_dictionary_set_uint64(read.get(), "size", 8);
        xpc_dictionary_set_uint64(read.get(), "timeout", 100);
        CHECK(codeOf(send(read)) == ErrorCode::illegalState); // nothing claimed

        auto other = makeRequest("claim");
        putIdentity(other.get(), {0x1234, 1, 1, 5});
        CHECK(codeOf(send(other)) == ErrorCode::invalidArgument);
        auto android = makeRequest("claim"); // a phone booted into Android
        putIdentity(android.get(), {0x04E8, 0x6860, 1, 5});
        CHECK(codeOf(send(android)) == ErrorCode::invalidArgument);

        auto claim = makeRequest("claim");
        putIdentity(claim.get(), sim::Transport::identity());
        CHECK(send(claim));
        CHECK(codeOf(send(claim)) == ErrorCode::illegalState); // one claim per connection

        auto huge = makeRequest("read");
        xpc_dictionary_set_uint64(huge.get(), "size", kMaximumTransfer + 1);
        xpc_dictionary_set_uint64(huge.get(), "timeout", 100);
        CHECK(codeOf(send(huge)) == ErrorCode::invalidArgument);

        auto noTimeout = makeRequest("zlp");
        CHECK(codeOf(send(noTimeout)) == ErrorCode::invalidArgument);

        auto stale = Xpc::adopt(xpc_dictionary_create(nullptr, nullptr, 0));
        xpc_dictionary_set_string(stale.get(), "op", "enumerate");
        xpc_dictionary_set_uint64(stale.get(), "version", kWireVersion + 1);
        CHECK(codeOf(send(stale)) == ErrorCode::protocolViolation);

        CHECK(codeOf(send(makeRequest("format-disk"))) == ErrorCode::invalidArgument);

        // A client that disappears releases its claim.
        xpc_connection_cancel(static_cast<xpc_connection_t>(connection->get()));
        CHECK(waitUntil([&] { return chain.device->released(); }));
    }

    // Hot-plug notifications travel from the helper.
    {
        Chain chain;
        auto remote = RemoteTransport::endpoint(chain.usbdListener->endpoint());
        std::mutex mutex;
        std::condition_variable changed;
        int count = 0;
        CHECK(remote->startHotplug([&] {
            std::lock_guard<std::mutex> lock(mutex);
            ++count;
            changed.notify_all();
        }));
        chain.simTransport->setAttached(false);
        std::unique_lock<std::mutex> lock(mutex);
        CHECK(changed.wait_for(lock, kWait, [&] { return count > 0; }));
        lock.unlock();
        remote->stopHotplug();
    }

    // A helper that fails its code-signing requirement is refused.
    {
        Chain chain;
        auto remote = RemoteTransport::endpoint(chain.usbdListener->endpoint(),
                                                "identifier \"nonexistent.bifrost.peer\"");
        auto devices = remote->enumerate();
        CHECK(!devices && devices.error().code == ErrorCode::serviceUnavailable);
        if (!devices) {
            CHECK_CONTAINS(devices.error().message, "code-signing");
        }
        auto claimed = remote->claim(sim::Transport::identity());
        CHECK(!claimed && chain.device->commands().empty());
    }

    // An unreachable helper is a service error, not a crash.
    {
        Chain chain;
        auto endpoint = chain.usbdListener->endpoint();
        chain.usbdListener.reset();
        auto remote = RemoteTransport::endpoint(endpoint);
        auto devices = remote->enumerate();
        CHECK(!devices && devices.error().code == ErrorCode::serviceUnavailable);
    }

    // ---- agent -------------------------------------------------------------

    // Full flash through agent and helper, with events and final status.
    {
        Chain chain;
        Events events;
        auto client = chain.client(events.handler());
        CHECK(client && client->subscribe());
        auto devices = client->devices();
        CHECK(devices && devices->devices.size() == 1);
        auto none = client->status();
        CHECK(none && !none->has_value());

        auto job = client->startFlash(flashRequest(small));
        CHECK(job && *job > 0);
        auto finished = events.waitFor(AgentEvent::Kind::finished);
        CHECK(finished && finished->result && !finished->result->error);
        CHECK(finished && finished->result &&
              finished->result->bytesTransferred == small.boot.size() + small.system.size());
        CHECK(events.saw(AgentEvent::Kind::started));
        CHECK(events.saw(AgentEvent::Kind::plan, [](const AgentEvent& e) {
            return e.plan && e.plan->files.size() == 2 && e.plan->flashable;
        }));
        CHECK(events.saw(AgentEvent::Kind::progress, [](const AgentEvent& e) {
            return e.progress.phase == flash::Phase::committing;
        }));
        checkWritten(*chain.device, small);

        auto status = client->status();
        CHECK(status && status->has_value());
        if (status && status->has_value()) {
            const auto& s = **status;
            CHECK(!s.running && s.kind == JobKind::flash && s.result && !s.result->error);
            CHECK(s.plan && s.plan->detectedModel == "N950F");
            CHECK(!s.log.empty() && s.log.back().message.find("Finished") != std::string::npos);
        }
    }

    // UI crash mid-flash: the job keeps running; a new client reattaches.
    {
        Gate gate(2);
        auto config = Chain::baseConfig();
        config.onPart = gate.hook();
        Chain chain(config);
        {
            auto first = chain.client();
            CHECK(first && first->startFlash(flashRequest(small)));
            CHECK(gate.waitReached());
        } // first client gone, as if the UI crashed

        Events events;
        auto second = chain.client(events.handler());
        CHECK(second && second->subscribe());
        auto status = second->status();
        CHECK(status && status->has_value());
        if (status && status->has_value()) {
            const auto& s = **status;
            CHECK(s.running && s.kind == JobKind::flash && !s.result);
            CHECK(s.plan && s.plan->flashable);
            CHECK(s.progress.phase == flash::Phase::transferring);
            CHECK(s.device == sim::Transport::identity());
        }
        auto busy = second->startProbe(sim::Transport::identity());
        CHECK(!busy && busy.error().code == ErrorCode::illegalState);

        gate.open();
        auto finished = events.waitFor(AgentEvent::Kind::finished);
        CHECK(finished && finished->result && !finished->result->error);
        checkWritten(*chain.device, small);
    }

    // The agent reads through the descriptor: replacing the path mid-flash
    // does not change what is written.
    {
        Gate gate(0);
        auto config = Chain::baseConfig();
        config.onPart = gate.hook();
        Chain chain(config);
        const auto copy = makeFirmware(dir, "SWAP", 2 * 1024 * 1024 + 5);
        Events events;
        auto client = chain.client(events.handler());
        CHECK(client && client->subscribe());
        CHECK(client->startFlash(flashRequest(copy)));
        CHECK(gate.waitReached());
        const auto replacement = dir.write("replacement.tar.md5", Bytes(4096, 0xEE));
        CHECK(std::rename(replacement.c_str(), copy.path.c_str()) == 0);
        gate.open();
        auto finished = events.waitFor(AgentEvent::Kind::finished);
        CHECK(finished && finished->result && !finished->result->error);
        checkWritten(*chain.device, copy);
    }

    // "Stop after this sequence" through the agent.
    {
        Gate gate(0);
        auto config = Chain::baseConfig();
        config.onPart = gate.hook();
        Chain chain(config);
        Events events;
        auto client = chain.client(events.handler());
        CHECK(client && client->subscribe());
        CHECK(client->startFlash(flashRequest(large)));
        CHECK(gate.waitReached());
        CHECK(client->cancel(CancelLevel::afterSequence));
        gate.open();
        auto finished = events.waitFor(AgentEvent::Kind::finished);
        CHECK(finished && finished->result && finished->result->error &&
              finished->result->error->code == ErrorCode::cancelled &&
              finished->result->error->dataState == DataState::partial);
        CHECK(chain.device->written(1) && !chain.device->written(2));
        auto idle = client->cancel(CancelLevel::beforeData);
        CHECK(!idle && idle.error().code == ErrorCode::illegalState);
    }

    // Refusals and validation.
    {
        Chain chain;
        Events events;
        auto client = chain.client(events.handler());
        CHECK(client && client->subscribe());
        auto bad = flashRequest(small);
        bad.device.vendorId = 0x1234;
        auto refused = client->startFlash(bad);
        CHECK(!refused && refused.error().code == ErrorCode::invalidArgument);
        auto noModel = client->startFlash(flashRequest(small, ""));
        CHECK(!noModel && noModel.error().code == ErrorCode::invalidArgument);
        auto missing = flashRequest(small);
        missing.packages[0].path = dir.write("x", {}) + ".missing";
        CHECK(!client->startFlash(missing));

        CHECK(client->startFlash(flashRequest(small, "SM-G973F")));
        auto finished = events.waitFor(AgentEvent::Kind::finished);
        CHECK(finished && finished->result && finished->result->error &&
              finished->result->error->code == ErrorCode::modelMismatch &&
              finished->result->error->dataState == DataState::none);
        CHECK(chain.device->dataBytesReceived() == 0);
    }

    // Probe and PIT read jobs.
    {
        Chain chain;
        Events probeEvents;
        auto client = chain.client(probeEvents.handler());
        CHECK(client && client->subscribe());
        CHECK(client->startProbe(sim::Transport::identity()));
        auto probed = probeEvents.waitFor(AgentEvent::Kind::finished);
        CHECK(probed && probed->result && !probed->result->error &&
              probed->result->session.protocolVersion == 2);
    }
    {
        Chain chain;
        Events events;
        auto client = chain.client(events.handler());
        CHECK(client && client->subscribe());
        CHECK(client->startReadPit(sim::Transport::identity()));
        auto read = events.waitFor(AgentEvent::Kind::finished);
        CHECK(read && read->result && !read->result->error &&
              read->result->pit == test::makePit(kLayout));
    }

    // SIGTERM path: shutdown stops a running flash at the next safe point.
    {
        Gate gate(0);
        auto config = Chain::baseConfig();
        config.onPart = gate.hook();
        Chain chain(config);
        auto client = chain.client();
        CHECK(client && client->startFlash(flashRequest(large)));
        CHECK(gate.waitReached());
        std::thread stopper([&] { chain.agent->shutdown(); });
        CHECK(waitUntil([&] {
            auto s = client->status();
            return s && s->has_value() &&
                   std::any_of((*s)->log.begin(), (*s)->log.end(), [](const LogLine& line) {
                       return line.message.find("shutting down") != std::string::npos;
                   });
        }));
        gate.open();
        stopper.join();
        CHECK(!chain.agent->jobRunning());
        auto status = client->status();
        CHECK(status && status->has_value() && (*status)->result && (*status)->result->error &&
              (*status)->result->error->code == ErrorCode::cancelled);
        auto after = client->startProbe(sim::Transport::identity());
        CHECK(!after && after.error().code == ErrorCode::serviceUnavailable);
    }

    // An agent that fails the client's requirement is refused.
    {
        Chain chain;
        auto client = AgentClient::endpoint(chain.agentListener->endpoint(), {},
                                            "identifier \"nonexistent.bifrost.agent\"");
        CHECK(!client && client.error().code == ErrorCode::serviceUnavailable);
    }

    return test::result();
}
