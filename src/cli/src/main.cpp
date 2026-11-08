// bifrost-cli: command-line front end for the Bifrost Studio core.

#include "bifrost/firmware/naming.hpp"
#include "bifrost/firmware/package.hpp"
#include "bifrost/flash/flash.hpp"
#include "bifrost/pit/pit.hpp"
#include "bifrost/planner/plan.hpp"
#include "bifrost/version.h"

#if BIFROST_HAS_USB
#include "bifrost/transport/libusb_transport.hpp"
#endif
#if BIFROST_HAS_AGENT
#include "agent_commands.hpp"
#endif

#include <algorithm>
#include <cerrno>
#include <charconv>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <iterator>
#include <limits>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <unistd.h>
#include <vector>

namespace {

using namespace bifrost;

constexpr int kExitOk = 0;
constexpr int kExitFailure = 1;
constexpr int kExitUsage = 2;

const char* kUsage =
    "Usage:\n"
    "  bifrost-cli --version\n"
    "  bifrost-cli devices [--machine]\n"
    "  bifrost-cli claim-test --bus N --address N\n"
    "  bifrost-cli probe --bus N --address N [--resume]\n"
    "  bifrost-cli read-pit --bus N --address N --output PATH [--resume]\n"
    "  bifrost-cli print-pit PATH\n"
    "  bifrost-cli inspect PACKAGE\n"
    "  bifrost-cli plan --pit PATH SLOTS\n"
    "  bifrost-cli flash --bus N --address N --product-id N --model MODEL\n"
    "                    [--accept-data-erase] [--no-reboot] [--review-pit PATH]\n"
    "                    [--machine] SLOTS\n"
    "  bifrost-cli agent ...   (run 'bifrost-cli agent' for the background-agent commands)\n"
    "--resume skips the handshake: the bootloader answers it once per entry into\n"
    "Download Mode, so use it for probe/read-pit after another session on the same\n"
    "entry. flash never resumes: re-enter Download Mode before flashing.\n"
    "SLOTS: --bl PATH --ap PATH --cp PATH --csc PATH --userdata PATH\n"
    "flash uses one Odin session: it reads the device PIT, builds the plan, and\n"
    "refuses on any ambiguity before sending data. --model must match the model\n"
    "on the Download Mode screen. Re-partition and NAND erase are unsupported.\n";

// ---- argument parsing -------------------------------------------------------

struct Arguments {
    std::map<std::string, std::string> values;
    std::vector<std::string> flags;
    std::vector<flash::SlotPath> slots;
};

const std::vector<std::string> kValueOptions{
    "--bus", "--address", "--product-id", "--output", "--pit",
    "--model", "--review-pit"};
const std::vector<std::string> kFlagOptions{
    "--machine", "--accept-data-erase", "--no-reboot", "--resume"};

std::optional<Arguments> parseArguments(int argc, char** argv, int first)
{
    Arguments arguments;
    for (int i = first; i < argc; ++i) {
        const std::string option(argv[i]);
        const auto slot = option.size() > 2 && option.compare(0, 2, "--") == 0
                              ? planner::parseSlot(option.substr(2))
                              : std::nullopt;
        if (slot || std::find(kValueOptions.begin(), kValueOptions.end(), option) !=
                        kValueOptions.end()) {
            if (++i >= argc || argv[i][0] == '\0') {
                std::cerr << "Missing value after " << option << '\n';
                return std::nullopt;
            }
            if (slot) {
                const bool duplicate = std::any_of(
                    arguments.slots.begin(), arguments.slots.end(),
                    [&](const flash::SlotPath& s) { return s.slot == *slot; });
                if (duplicate) {
                    std::cerr << option << " was given more than once\n";
                    return std::nullopt;
                }
                arguments.slots.push_back({*slot, argv[i]});
            } else if (!arguments.values.emplace(option, argv[i]).second) {
                std::cerr << option << " was given more than once\n";
                return std::nullopt;
            }
        } else if (std::find(kFlagOptions.begin(), kFlagOptions.end(), option) !=
                   kFlagOptions.end()) {
            arguments.flags.push_back(option);
        } else {
            std::cerr << "Unknown option: " << option << '\n';
            return std::nullopt;
        }
    }
    return arguments;
}

bool hasFlag(const Arguments& arguments, const std::string& flag)
{
    return std::find(arguments.flags.begin(), arguments.flags.end(), flag) !=
           arguments.flags.end();
}

template <typename Integer>
std::optional<Integer> parseInteger(const Arguments& arguments, const std::string& key)
{
    const auto found = arguments.values.find(key);
    if (found == arguments.values.end()) {
        return std::nullopt;
    }
    unsigned long value = 0;
    const auto& text = found->second;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    if (result.ec != std::errc{} || result.ptr != text.data() + text.size() ||
        value > std::numeric_limits<Integer>::max()) {
        return std::nullopt;
    }
    return static_cast<Integer>(value);
}

std::string valueOr(const Arguments& arguments, const std::string& key)
{
    const auto found = arguments.values.find(key);
    return found == arguments.values.end() ? std::string() : found->second;
}

// ---- output -----------------------------------------------------------------

int reportError(const std::string& context, const Error& error)
{
    std::cerr << context << ": " << error.message << "\n  [" << toString(error.code)
              << "; " << toString(error.dataState) << "]\n";
    switch (error.dataState) {
    case DataState::none:
        std::cerr << "No partition data was sent. If the session was opened, "
                     "re-enter Download Mode before retrying.\n";
        break;
    case DataState::partial:
        std::cerr << "The device may be PARTIALLY WRITTEN. Do not reboot it. Keep "
                     "it in Download Mode and flash complete, matching firmware "
                     "again.\n";
        break;
    case DataState::complete:
        std::cerr << "All image data was acknowledged; only closing the session "
                     "or rebooting failed. Check the device screen before "
                     "rebooting it manually.\n";
        break;
    }
    return kExitFailure;
}

std::string bytes(std::uint64_t value)
{
    const char* units[] = {"B", "KiB", "MiB", "GiB"};
    double scaled = static_cast<double>(value);
    int unit = 0;
    while (scaled >= 1024 && unit < 3) {
        scaled /= 1024;
        ++unit;
    }
    std::ostringstream text;
    text << std::fixed << std::setprecision(unit == 0 ? 0 : 1) << scaled << ' '
         << units[unit];
    return text.str();
}

void printPlan(const planner::FlashPlan& plan)
{
    std::cout << "Detected firmware model: "
              << (plan.detectedModel.empty() ? "unknown" : "SM-" + plan.detectedModel);
    if (plan.bootloaderRevision) {
        std::cout << " (bootloader revision " << *plan.bootloaderRevision << ")";
    }
    std::cout << '\n';
    for (const auto& file : plan.files) {
        std::cout << "  " << std::setw(8) << std::left << planner::toString(file.slot)
                  << file.entry.name << " -> " << file.partition.partitionName
                  << " (" << bytes(file.entry.size)
                  << (file.entry.lz4Compressed ? ", LZ4" : "") << ")\n";
    }
    for (const auto& skipped : plan.skipped) {
        std::cout << "  skipped  " << skipped.entry << " (" << skipped.reason << ")\n";
    }
    for (const auto& warning : plan.warnings) {
        std::cout << (warning.requiresAcknowledgement ? "  WARNING* " : "  warning  ")
                  << warning.message << '\n';
    }
    for (const auto& refusal : plan.refusals) {
        std::cout << "  REFUSED  " << refusal << '\n';
    }
    if (plan.requiresAcknowledgement()) {
        std::cout << "  * requires --accept-data-erase\n";
    }
}

Expected<pit::Table> loadPitFile(const std::string& path)
{
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return makeError(ErrorCode::ioError, "Could not open " + path);
    }
    std::vector<std::uint8_t> data((std::istreambuf_iterator<char>(input)),
                                   std::istreambuf_iterator<char>());
    return pit::parse(data.data(), data.size());
}

// ---- offline commands -------------------------------------------------------

int printPit(const std::string& path)
{
    auto table = loadPitFile(path);
    if (!table) {
        return reportError("PIT", table.error());
    }
    std::cout << "PIT entries: " << table->entries.size() << '\n';
    for (const auto& entry : table->entries) {
        std::cout << "ID " << std::setw(4) << entry.identifier << "  "
                  << std::setw(16) << std::left << entry.partitionName << std::right
                  << " flash=" << (entry.flashFilename.empty() ? "-" : entry.flashFilename)
                  << "  binary=" << entry.binaryType << "  device=" << entry.deviceType
                  << "  blocks=" << entry.blockCount << '\n';
    }
    return kExitOk;
}

int inspect(const std::string& path)
{
    auto package = firmware::inspectPackage(path);
    if (!package) {
        return reportError("Package", package.error());
    }
    const auto name = firmware::parseFirmwareName(path);
    std::cout << "Package: " << path << '\n'
              << "Checksum: "
              << (package->checksumValid ? "MD5 verified (" + package->checksumHex + ")"
                                         : std::string("none (plain .tar)"))
              << '\n'
              << "Kind: " << firmware::toString(name.kind) << '\n'
              << "Build: " << (name.build.empty() ? "unknown" : name.build) << '\n'
              << "Model: " << (name.modelCode.empty() ? "unknown" : "SM-" + name.modelCode)
              << '\n';
    if (!name.cscCode.empty()) {
        std::cout << "CSC: " << name.cscCode << '\n';
    }
    if (name.bootloaderRevision) {
        std::cout << "Bootloader revision: " << *name.bootloaderRevision << '\n';
    }
    std::cout << "Entries:\n";
    for (const auto& entry : package->entries) {
        std::cout << "  " << std::setw(12) << bytes(entry.size) << "  " << entry.name
                  << (entry.lz4Compressed ? "  (LZ4)" : "") << '\n';
    }
    return kExitOk;
}

flash::Observer verboseObserver(bool machine)
{
    flash::Observer observer;
    observer.onLog = [](flash::LogLevel level, const std::string& message) {
        std::cerr << (level == flash::LogLevel::info ? "" : "Warning: ") << message << '\n';
    };
    if (machine) {
        observer.onProgress = [](const flash::Progress& progress) {
            std::string entry = progress.entry;
            std::replace_if(entry.begin(), entry.end(),
                            [](char c) { return c == '\t' || c == '\n' || c == '\r'; },
                            '_');
            std::cout << "PROGRESS\t" << flash::toString(progress.phase) << '\t'
                      << progress.slot << '\t' << entry << '\t' << progress.fileDone
                      << '\t' << progress.fileTotal << '\t' << progress.totalDone
                      << '\t' << progress.total << std::endl;
        };
    }
    return observer;
}

int plan(const Arguments& arguments)
{
    const auto pitPath = valueOr(arguments, "--pit");
    if (pitPath.empty() || arguments.slots.empty()) {
        std::cerr << "plan requires --pit PATH and at least one slot\n";
        return kExitUsage;
    }
    auto table = loadPitFile(pitPath);
    if (!table) {
        return reportError("PIT", table.error());
    }
    auto packages = flash::loadPackages(arguments.slots, verboseObserver(false));
    if (!packages) {
        return reportError("Package", packages.error());
    }
    std::cout << "OFFLINE PLAN - no device was accessed.\n";
    const auto result = planner::buildPlan(*table, *packages);
    printPlan(result);
    return result.flashable() ? kExitOk : kExitFailure;
}

#if BIFROST_HAS_USB

// ---- device commands --------------------------------------------------------

struct Device {
    std::unique_ptr<transport::LibusbTransport> transport;
    std::unique_ptr<transport::ClaimedInterface> interface;
};

std::optional<transport::UsbDeviceInfo> findDevice(transport::UsbTransport& usb,
                                                   const Arguments& arguments,
                                                   bool requireProductId)
{
    const auto bus = parseInteger<std::uint8_t>(arguments, "--bus");
    const auto address = parseInteger<std::uint8_t>(arguments, "--address");
    const auto productId = parseInteger<std::uint16_t>(arguments, "--product-id");
    if (!bus || !address || (requireProductId && !productId)) {
        std::cerr << "--bus and --address (and --product-id for flash) are required "
                     "decimal integers\n";
        return std::nullopt;
    }
    auto enumeration = usb.enumerate();
    if (!enumeration) {
        reportError("USB", enumeration.error());
        return std::nullopt;
    }
    for (const auto& device : enumeration->devices) {
        if (device.identity.busNumber == *bus && device.identity.deviceAddress == *address &&
            (!productId || device.identity.productId == *productId)) {
            return device;
        }
    }
    std::cerr << "No matching Samsung device; run 'devices' again\n";
    return std::nullopt;
}

std::optional<Device> openDevice(const Arguments& arguments, bool requireProductId)
{
    auto usb = transport::LibusbTransport::create();
    if (!usb) {
        reportError("USB", usb.error());
        return std::nullopt;
    }
    auto device = findDevice(**usb, arguments, requireProductId);
    if (!device) {
        return std::nullopt;
    }
    auto claimed = (*usb)->claim(device->identity);
    if (!claimed) {
        reportError("Claim", claimed.error());
        return std::nullopt;
    }
    return Device{std::move(*usb), std::move(*claimed)};
}

int devices(bool machine)
{
    auto usb = transport::LibusbTransport::create();
    if (!usb) {
        return reportError("USB", usb.error());
    }
    auto enumeration = (*usb)->enumerate();
    if (!enumeration) {
        return reportError("USB", enumeration.error());
    }
    if (!machine) {
        for (const auto& warning : enumeration->warnings) {
            std::cerr << "Warning: " << warning << '\n';
        }
        if (enumeration->devices.empty()) {
            std::cout << "No Samsung USB devices found.\n";
        }
    }
    for (const auto& device : enumeration->devices) {
        const auto& id = device.identity;
        if (machine) {
            if (device.odinInterface) {
                std::cout << unsigned(id.busNumber) << '\t' << unsigned(id.deviceAddress)
                          << '\t' << id.productId << '\t'
                          << unsigned(device.odinInterface->interfaceNumber) << '\t'
                          << unsigned(device.odinInterface->alternateSetting) << '\n';
            }
            continue;
        }
        std::cout << std::hex << std::setfill('0') << "04e8:" << std::setw(4)
                  << id.productId << std::dec << std::setfill(' ') << " bus "
                  << unsigned(id.busNumber) << " address " << unsigned(id.deviceAddress)
                  << (device.odinInterface ? "  Download Mode interface found"
                                           : "  not in Download Mode (or no Odin interface)")
                  << '\n';
    }
    return kExitOk;
}

int claimTest(const Arguments& arguments)
{
    auto device = openDevice(arguments, false);
    if (!device) {
        return kExitFailure;
    }
    const auto& selection = device->interface->selection();
    std::cout << "Claimed interface " << unsigned(selection.interfaceNumber)
              << " alternate " << unsigned(selection.alternateSetting) << std::hex
              << " (IN 0x" << unsigned(selection.endpointIn) << ", OUT 0x"
              << unsigned(selection.endpointOut) << std::dec
              << "). No protocol data was sent.\n";
    if (auto released = device->interface->release(); !released) {
        return reportError("Release", released.error());
    }
    std::cout << "Released.\n";
    return kExitOk;
}

int probe(const Arguments& arguments)
{
    auto device = openDevice(arguments, false);
    if (!device) {
        return kExitFailure;
    }
    auto info = flash::probe(*device->interface, {}, hasFlag(arguments, "--resume"));
    if (!info) {
        return reportError("Probe", info.error());
    }
    std::cout << "Session ACK: 0x" << std::hex << info->sessionAck << std::dec
              << "\nProtocol version: " << info->protocolVersion
              << "\nPart size: " << info->packetSize << " bytes x "
              << info->partsPerSequence << " per sequence"
              << "\nCompressed transfer advertised: "
              << (info->supportsCompressedTransfer ? "yes" : "no")
              << "\nProbe complete; no PIT or firmware data was sent.\n";
    return kExitOk;
}

int readPit(const Arguments& arguments)
{
    const auto output = valueOr(arguments, "--output");
    if (output.empty()) {
        std::cerr << "read-pit requires --output PATH\n";
        return kExitUsage;
    }
    if (::access(output.c_str(), F_OK) == 0) {
        std::cerr << "Refusing to overwrite " << output << '\n';
        return kExitUsage;
    }
    auto device = openDevice(arguments, false);
    if (!device) {
        return kExitFailure;
    }
    auto raw = flash::readDevicePitBytes(*device->interface, {}, hasFlag(arguments, "--resume"));
    if (!raw) {
        return reportError("PIT", raw.error());
    }
    const std::vector<std::uint8_t> bytes = std::move(*raw);
    const int descriptor = ::open(output.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0644);
    if (descriptor < 0) {
        std::cerr << "Could not create " << output << ": " << std::strerror(errno) << '\n';
        return kExitFailure;
    }
    std::size_t written = 0;
    while (written < bytes.size()) {
        const auto count =
            ::write(descriptor, bytes.data() + written, bytes.size() - written);
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count <= 0) {
            ::close(descriptor);
            ::unlink(output.c_str());
            std::cerr << "Could not write " << output << '\n';
            return kExitFailure;
        }
        written += static_cast<std::size_t>(count);
    }
    if (::close(descriptor) != 0) {
        ::unlink(output.c_str());
        std::cerr << "Could not write " << output << '\n';
        return kExitFailure;
    }
    auto table = pit::parse(bytes.data(), bytes.size());
    if (!table) {
        std::cout << "Saved the raw PIT (" << bytes.size() << " bytes) to " << output
                  << " for diagnosis, but it failed validation.\n";
        return reportError("PIT", table.error());
    }
    std::cout << "Saved " << table->entries.size() << " PIT entries to " << output
              << ". This used its own read-only session.\n";
    return kExitOk;
}

int flashCommand(const Arguments& arguments)
{
    const auto model = valueOr(arguments, "--model");
    if (model.empty() || arguments.slots.empty()) {
        std::cerr << "flash requires --model MODEL and at least one slot.\n"
                     "Type the model shown on the device's Download Mode screen.\n";
        return kExitUsage;
    }
    if (hasFlag(arguments, "--resume")) {
        std::cerr << "flash cannot --resume: it must be the first session after entering\n"
                     "Download Mode. Re-enter Download Mode, then flash.\n";
        return kExitUsage;
    }
    const bool machine = hasFlag(arguments, "--machine");
    auto observer = verboseObserver(machine);
    observer.onPlan = [](const planner::FlashPlan& plan) {
        std::cout << "Plan built against the connected device:\n";
        printPlan(plan);
    };

    auto packages = flash::loadPackages(arguments.slots, observer);
    if (!packages) {
        return reportError("Package", packages.error());
    }
    flash::Request request;
    request.packages = std::move(*packages);
    request.confirmation = {model, hasFlag(arguments, "--accept-data-erase")};
    if (const auto reviewPit = valueOr(arguments, "--review-pit"); !reviewPit.empty()) {
        auto table = loadPitFile(reviewPit);
        if (!table) {
            return reportError("PIT", table.error());
        }
        request.reviewed = planner::buildPlan(*table, request.packages);
    }

    auto device = openDevice(arguments, true);
    if (!device) {
        return kExitFailure;
    }
    flash::Options options;
    options.reboot = !hasFlag(arguments, "--no-reboot");
    auto report = flash::flash(*device->interface, request, options, observer);
    if (!report) {
        return reportError("Flash", report.error());
    }
    std::cout << "Flash complete: " << bytes(report->bytesTransferred)
              << " transferred. "
              << (report->rebooted ? "Reboot requested." : "Device remains in Download Mode.")
              << '\n';
    return kExitOk;
}

#endif // BIFROST_HAS_USB

} // namespace

int main(int argc, char** argv)
{
    // A closed stdout (e.g. the app quitting) must never kill a running flash.
    std::signal(SIGPIPE, SIG_IGN);

    if (argc < 2) {
        std::cerr << kUsage;
        return kExitUsage;
    }
    const std::string command(argv[1]);
    if (command == "--version") {
        std::cout << "bifrost-cli " << bifrost_core_version() << '\n';
        return kExitOk;
    }
    if (command == "print-pit" && argc == 3) {
        return printPit(argv[2]);
    }
    if (command == "inspect" && argc == 3) {
        return inspect(argv[2]);
    }
#if BIFROST_HAS_AGENT
    if (command == "agent") {
        return agentCommand(argc, argv);
    }
#endif
    const auto arguments = parseArguments(argc, argv, 2);
    if (!arguments) {
        std::cerr << kUsage;
        return kExitUsage;
    }
    if (command == "plan" || command == "dry-run") {
        return plan(*arguments);
    }
#if BIFROST_HAS_USB
    if (command == "devices" || command == "list-devices") {
        return devices(hasFlag(*arguments, "--machine"));
    }
    if (command == "claim-test") {
        return claimTest(*arguments);
    }
    if (command == "probe" || command == "probe-device") {
        return probe(*arguments);
    }
    if (command == "read-pit" || command == "download-pit") {
        return readPit(*arguments);
    }
    if (command == "flash") {
        return flashCommand(*arguments);
    }
#endif
    std::cerr << kUsage;
    return kExitUsage;
}
