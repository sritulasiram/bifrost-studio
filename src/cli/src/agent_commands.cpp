// `bifrost-cli agent ...`: drive the installed background agent (and through
// it the root USB helper) instead of using libusb in this process.
#include "agent_commands.hpp"

#include "bifrost/pit/pit.hpp"
#include "bifrost/service/agent_client.hpp"
#include "bifrost/service/listener.hpp"
#include "service_config.hpp"

#include <charconv>
#include <condition_variable>
#include <cstdio>
#include <fcntl.h>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <unistd.h>
#include <vector>

namespace {

using namespace bifrost;
using namespace bifrost::service;

constexpr int kOk = 0;
constexpr int kFailure = 1;
constexpr int kUsage = 2;

const char* kAgentUsage =
    "Usage (via the installed background agent):\n"
    "  bifrost-cli agent status\n"
    "  bifrost-cli agent devices\n"
    "  bifrost-cli agent watch\n"
    "  bifrost-cli agent probe --bus N --address N [--resume]\n"
    "  bifrost-cli agent read-pit --bus N --address N --output PATH [--resume]\n"
    "  bifrost-cli agent flash --bus N --address N --model MODEL [--accept-data-erase]\n"
    "                          [--no-reboot] [--review-pit PATH] SLOTS\n"
    "  bifrost-cli agent cancel --level before-data|after-sequence\n"
    "Ctrl-C only detaches: a running job continues in the agent.\n";

struct Options {
    std::map<std::string, std::string> values;
    std::vector<std::string> flags;
    std::vector<PackagePath> packages;
};

std::optional<Options> parse(int argc, char** argv, int first)
{
    static const std::vector<std::string> valueOptions{"--bus", "--address", "--output", "--model",
                                                       "--review-pit", "--level"};
    static const std::vector<std::string> flagOptions{"--accept-data-erase", "--no-reboot",
                                                      "--resume"};
    Options options;
    for (int i = first; i < argc; ++i) {
        const std::string option = argv[i];
        const auto slot = option.size() > 2 && option.compare(0, 2, "--") == 0
                              ? planner::parseSlot(option.substr(2))
                              : std::nullopt;
        const bool takesValue =
            slot || std::find(valueOptions.begin(), valueOptions.end(), option) != valueOptions.end();
        if (takesValue) {
            if (++i >= argc || argv[i][0] == '\0') {
                std::cerr << "Missing value after " << option << '\n';
                return std::nullopt;
            }
            if (slot) {
                options.packages.push_back({*slot, argv[i]});
            } else if (!options.values.emplace(option, argv[i]).second) {
                std::cerr << option << " was given more than once\n";
                return std::nullopt;
            }
        } else if (std::find(flagOptions.begin(), flagOptions.end(), option) != flagOptions.end()) {
            options.flags.push_back(option);
        } else {
            std::cerr << "Unknown option: " << option << '\n';
            return std::nullopt;
        }
    }
    return options;
}

bool has(const Options& o, const char* flag)
{
    return std::find(o.flags.begin(), o.flags.end(), flag) != o.flags.end();
}

std::string value(const Options& o, const char* key)
{
    const auto found = o.values.find(key);
    return found == o.values.end() ? std::string() : found->second;
}

std::optional<unsigned> number(const Options& o, const char* key, unsigned maximum)
{
    const auto text = value(o, key);
    unsigned result = 0;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), result);
    if (text.empty() || parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() ||
        result > maximum) {
        return std::nullopt;
    }
    return result;
}

int fail(const char* what, const Error& error)
{
    std::cerr << what << " failed (" << toString(error.code) << ", " << toString(error.dataState)
              << "): " << error.message << '\n';
    return kFailure;
}

void printProgress(const flash::Progress& p)
{
    const double percent = p.total > 0 ? 100.0 * static_cast<double>(p.totalDone) /
                                             static_cast<double>(p.total)
                                       : 0.0;
    std::fprintf(stderr, "\r[%s] %s %s %llu/%llu  %.1f%%      ", flash::toString(p.phase),
                 p.slot.c_str(), p.entry.c_str(), static_cast<unsigned long long>(p.fileDone),
                 static_cast<unsigned long long>(p.fileTotal), percent);
}

void printPlan(const PlanSummary& plan)
{
    std::cout << "\nPlan (model " << (plan.detectedModel.empty() ? "?" : plan.detectedModel)
              << "):\n";
    for (const auto& f : plan.files) {
        std::cout << "  [" << planner::toString(f.slot) << "] " << f.entry << " -> "
                  << f.partition << " (id " << f.partitionId << ")\n";
    }
    for (const auto& s : plan.skipped) {
        std::cout << "  skipped " << s.entry << ": " << s.reason << '\n';
    }
    for (const auto& w : plan.warnings) {
        std::cout << "  warning: " << w.message << '\n';
    }
    for (const auto& r : plan.refusals) {
        std::cout << "  REFUSED: " << r << '\n';
    }
}

void printResult(const JobResult& result)
{
    if (result.error) {
        std::cout << "\nResult: FAILED (" << toString(result.error->code) << ", "
                  << toString(result.error->dataState) << "): " << result.error->message << '\n';
        return;
    }
    std::cout << "\nResult: OK";
    if (result.session.packetSize != 0) {
        std::cout << "; protocol " << result.session.protocolVersion << ", session ACK 0x"
                  << std::hex << result.session.sessionAck << std::dec;
    }
    if (result.bytesTransferred != 0) {
        std::cout << "; " << result.bytesTransferred << " bytes transferred"
                  << (result.rebooted ? "; reboot requested" : "; device left in Download Mode");
    }
    std::cout << '\n';
}

// Collects events and lets the main thread wait for a job to finish.
class Waiter {
public:
    AgentEventHandler handler()
    {
        return [this](const AgentEvent& event) {
            switch (event.kind) {
            case AgentEvent::Kind::progress:
                printProgress(event.progress);
                break;
            case AgentEvent::Kind::log:
                std::fprintf(stderr, "\n%s\n", event.log.message.c_str());
                break;
            case AgentEvent::Kind::plan:
                if (event.plan) {
                    printPlan(*event.plan);
                }
                break;
            default:
                break;
            }
            std::lock_guard<std::mutex> lock(mutex_);
            if (event.kind == AgentEvent::Kind::finished && (job_ == 0 || event.job == job_)) {
                result_ = event.result;
                done_ = true;
            }
            if (event.kind == AgentEvent::Kind::disconnected) {
                disconnected_ = true;
            }
            changed_.notify_all();
        };
    }
    void expect(std::uint64_t job)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        job_ = job;
    }
    // Returns the result, or nothing if the agent went away.
    std::optional<JobResult> wait()
    {
        std::unique_lock<std::mutex> lock(mutex_);
        changed_.wait(lock, [this] { return done_ || disconnected_; });
        return done_ ? result_ : std::nullopt;
    }

private:
    std::mutex mutex_;
    std::condition_variable changed_;
    std::uint64_t job_{};
    bool done_{};
    bool disconnected_{};
    std::optional<JobResult> result_;
};

std::unique_ptr<AgentClient> connect(AgentEventHandler handler)
{
    auto client = AgentClient::machService(
        config::kAgentService,
        signingRequirement({config::kAgentIdentifier}, config::kTeamId), std::move(handler));
    if (!client) {
        fail("Connecting to the background agent", client.error());
        std::cerr << "Is Bifrost Studio's background service registered and approved in "
                     "System Settings > General > Login Items?\n";
        return nullptr;
    }
    return std::move(*client);
}

std::optional<transport::UsbIdentity> findDevice(AgentClient& client, const Options& options)
{
    const auto bus = number(options, "--bus", 255);
    const auto address = number(options, "--address", 255);
    if (!bus || !address) {
        std::cerr << "--bus and --address are required decimal integers\n";
        return std::nullopt;
    }
    auto devices = client.devices();
    if (!devices) {
        fail("Listing devices", devices.error());
        return std::nullopt;
    }
    for (const auto& device : devices->devices) {
        if (device.identity.busNumber == *bus && device.identity.deviceAddress == *address) {
            return device.identity;
        }
    }
    std::cerr << "No matching Samsung device; run 'agent devices' again\n";
    return std::nullopt;
}

int runJob(AgentClient& client, Waiter& waiter, Expected<std::uint64_t> started,
           const Options& options, bool savePit)
{
    if (!started) {
        return fail("Starting the job", started.error());
    }
    waiter.expect(*started);
    std::cerr << "Job " << *started << " started in the background agent.\n";
    auto result = waiter.wait();
    if (!result) {
        std::cerr << "\nLost the connection to the agent; check 'bifrost-cli agent status'.\n";
        return kFailure;
    }
    printResult(*result);
    if (savePit && !result->pit.empty()) {
        const auto output = value(options, "--output");
        const int fd = ::open(output.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0644);
        if (fd < 0 || ::write(fd, result->pit.data(), result->pit.size()) !=
                          static_cast<ssize_t>(result->pit.size())) {
            if (fd >= 0) {
                ::close(fd);
                ::unlink(output.c_str());
            }
            std::cerr << "Could not write " << output << '\n';
            return kFailure;
        }
        ::close(fd);
        std::cout << "Saved " << (result->error ? "raw (unvalidated) " : "") << "PIT ("
                  << result->pit.size() << " bytes) to " << output << '\n';
    }
    (void)client;
    return result->error ? kFailure : kOk;
}

} // namespace

int agentCommand(int argc, char** argv)
{
    if (argc < 3) {
        std::cerr << kAgentUsage;
        return kUsage;
    }
    const std::string sub = argv[2];
    const auto options = parse(argc, argv, 3);
    if (!options) {
        std::cerr << kAgentUsage;
        return kUsage;
    }
    Waiter waiter;
    auto client = connect(waiter.handler());
    if (!client) {
        return kFailure;
    }

    if (sub == "status") {
        auto status = client->status();
        if (!status) {
            return fail("Status", status.error());
        }
        if (!status->has_value()) {
            std::cout << "No job has run since the agent started.\n";
            return kOk;
        }
        const auto& s = **status;
        std::cout << "Job " << s.id << " (" << toString(s.kind) << ") "
                  << (s.running ? "RUNNING" : "finished") << '\n';
        printProgress(s.progress);
        std::fprintf(stderr, "\n");
        if (s.plan) {
            printPlan(*s.plan);
        }
        for (const auto& line : s.log) {
            std::cout << "  " << line.message << '\n';
        }
        if (s.result) {
            printResult(*s.result);
        }
        return kOk;
    }
    if (sub == "devices") {
        auto devices = client->devices();
        if (!devices) {
            return fail("Listing devices", devices.error());
        }
        for (const auto& w : devices->warnings) {
            std::cerr << "Warning: " << w << '\n';
        }
        if (devices->devices.empty()) {
            std::cout << "No Samsung USB devices found.\n";
        }
        for (const auto& d : devices->devices) {
            std::fprintf(stdout, "04e8:%04x bus %u address %u  %s\n", d.identity.productId,
                         d.identity.busNumber, d.identity.deviceAddress,
                         d.odinInterface ? "Download Mode interface found"
                                         : "not in Download Mode (or no Odin interface)");
        }
        return kOk;
    }
    if (sub == "watch") {
        if (auto subscribed = client->subscribe(); !subscribed) {
            return fail("Subscribing", subscribed.error());
        }
        auto status = client->status();
        if (!status || !status->has_value() || !(*status)->running) {
            std::cout << "No job is running.\n";
            return kOk;
        }
        waiter.expect((*status)->id);
        auto result = waiter.wait();
        if (result) {
            printResult(*result);
        }
        return result && !result->error ? kOk : kFailure;
    }
    if (sub == "cancel") {
        const auto level = value(*options, "--level");
        CancelLevel parsed;
        if (level == "before-data") {
            parsed = CancelLevel::beforeData;
        } else if (level == "after-sequence") {
            parsed = CancelLevel::afterSequence;
            std::cerr << "Stopping after the current sequence leaves the device partially "
                         "written.\n";
        } else {
            std::cerr << kAgentUsage;
            return kUsage;
        }
        auto cancelled = client->cancel(parsed);
        return cancelled ? kOk : fail("Cancel", cancelled.error());
    }

    if (auto subscribed = client->subscribe(); !subscribed) {
        return fail("Subscribing", subscribed.error());
    }
    if (sub == "probe") {
        auto device = findDevice(*client, *options);
        return device ? runJob(*client, waiter, client->startProbe(*device, has(*options, "--resume")), *options, false)
                      : kFailure;
    }
    if (sub == "read-pit") {
        const auto output = value(*options, "--output");
        if (output.empty() || ::access(output.c_str(), F_OK) == 0) {
            std::cerr << "read-pit needs --output PATH that does not exist yet\n";
            return kUsage;
        }
        auto device = findDevice(*client, *options);
        return device ? runJob(*client, waiter, client->startReadPit(*device, has(*options, "--resume")), *options, true)
                      : kFailure;
    }
    if (sub == "flash") {
        FlashJobRequest request;
        request.typedModel = value(*options, "--model");
        request.packages = options->packages;
        request.acceptDataErase = has(*options, "--accept-data-erase");
        request.reboot = !has(*options, "--no-reboot");
        if (has(*options, "--resume")) {
            std::cerr << "flash cannot --resume: re-enter Download Mode, then flash\n";
            return kUsage;
        }
        if (request.typedModel.empty() || request.packages.empty()) {
            std::cerr << "flash requires --model MODEL and at least one slot\n";
            return kUsage;
        }
        if (const auto review = value(*options, "--review-pit"); !review.empty()) {
            std::ifstream file(review, std::ios::binary);
            request.reviewedPit.assign(std::istreambuf_iterator<char>(file), {});
            if (request.reviewedPit.empty() ||
                !pit::parse(request.reviewedPit.data(), request.reviewedPit.size())) {
                std::cerr << "Could not read a valid PIT from " << review << '\n';
                return kFailure;
            }
        }
        auto device = findDevice(*client, *options);
        if (!device) {
            return kFailure;
        }
        request.device = *device;
        return runJob(*client, waiter, client->startFlash(request), *options, false);
    }
    std::cerr << kAgentUsage;
    return kUsage;
}
