#include "bifrost/flash/flash.hpp"

#include "bifrost/firmware/lz4.hpp"

#include <limits>
#include <map>
#include <memory>
#include <utility>

namespace bifrost::flash {
namespace {

void log(const Observer& observer, LogLevel level, const std::string& message)
{
    if (observer.onLog) {
        observer.onLog(level, message);
    }
}

void report(const Observer& observer, const Progress& progress)
{
    if (observer.onProgress) {
        observer.onProgress(progress);
    }
}

// Closes a session that has not sent data, so a pre-transfer refusal leaves
// the device in a clean state. The original error is preserved.
Error abortCleanly(odin::Session& session, Error error)
{
    if (session.state() == odin::Session::State::open && !session.dataSent()) {
        if (auto closed = session.close(false); !closed) {
            error.message += " (session close also failed: " +
                             closed.error().message + ")";
        }
    }
    return error;
}

Status verifyFilesUnchanged(const std::vector<planner::SlotPackage>& packages)
{
    for (const auto& input : packages) {
        if (input.package && input.package->source) {
            if (auto unchanged = input.package->source->verifyUnchanged();
                !unchanged) {
                return unchanged;
            }
        }
    }
    return success();
}

} // namespace

const char* toString(Phase phase) noexcept
{
    switch (phase) {
    case Phase::verifying: return "verifying";
    case Phase::checkingImages: return "checking-images";
    case Phase::connecting: return "connecting";
    case Phase::readingPit: return "reading-pit";
    case Phase::planning: return "planning";
    case Phase::transferring: return "transferring";
    case Phase::committing: return "committing";
    case Phase::closing: return "closing";
    case Phase::done: return "done";
    }
    return "unknown";
}

Expected<std::vector<planner::SlotPackage>> loadPackages(
    const std::vector<SlotPath>& paths, const Observer& observer,
    const CancelToken* cancel)
{
    std::vector<planner::SlotPackage> packages;
    for (const auto& item : paths) {
        const std::string slot = planner::toString(item.slot);
        Progress progress{Phase::verifying, slot, item.path, 0, 0, 0, 0};
        const auto onHash = [&](std::uint64_t done, std::uint64_t total) {
            progress.fileDone = done;
            progress.fileTotal = total;
            report(observer, progress);
        };
        auto package = item.source ? firmware::inspectPackage(item.source, onHash, cancel)
                                   : firmware::inspectPackage(item.path, onHash, cancel);
        if (!package) {
            Error error = std::move(package).error();
            error.message = "[" + slot + "] " + error.message;
            return error;
        }
        log(observer, LogLevel::info,
            "[" + slot + "] " + item.path +
                (package->checksumValid ? ": MD5 verified" : ": no MD5 (plain .tar)"));
        packages.push_back(
            {item.slot, std::make_shared<const firmware::Package>(std::move(*package))});
    }
    return packages;
}

Expected<odin::SessionInfo> probe(transport::ClaimedInterface& interface,
                                  const odin::Timeouts& timeouts, bool resume)
{
    odin::Session session(interface, timeouts);
    auto info = session.open(resume);
    if (!info) {
        return info;
    }
    if (auto closed = session.close(false); !closed) {
        return closed.error();
    }
    return info;
}

Expected<std::vector<std::uint8_t>> readDevicePitBytes(transport::ClaimedInterface& interface,
                                                       const odin::Timeouts& timeouts,
                                                       bool resume)
{
    odin::Session session(interface, timeouts);
    if (auto opened = session.open(resume); !opened) {
        return std::move(opened).error();
    }
    auto bytes = session.readPit();
    if (!bytes) {
        return std::move(bytes).error(); // session state is uncertain; left open
    }
    if (auto closed = session.close(false); !closed) {
        return closed.error();
    }
    return bytes;
}

Expected<DevicePit> readDevicePit(transport::ClaimedInterface& interface,
                                  const odin::Timeouts& timeouts, bool resume)
{
    auto bytes = readDevicePitBytes(interface, timeouts, resume);
    if (!bytes) {
        return std::move(bytes).error();
    }
    auto table = pit::parse(bytes->data(), bytes->size());
    if (!table) {
        return std::move(table).error();
    }
    return DevicePit{std::move(*bytes), std::move(*table)};
}

Expected<Report> flash(transport::ClaimedInterface& interface, const Request& request,
                       const Options& options, const Observer& observer,
                       const CancelToken* cancel)
{
    if (request.packages.empty()) {
        return makeError(ErrorCode::invalidArgument, "Select at least one package");
    }
    if (auto unchanged = verifyFilesUnchanged(request.packages); !unchanged) {
        return unchanged.error();
    }

    // Decode every LZ4 image once before touching the device, so a corrupt
    // image is rejected before any data is sent, and its exact size is known.
    std::map<std::pair<const firmware::Package*, std::string>, std::uint64_t> lz4Sizes;
    for (const auto& input : request.packages) {
        for (const auto& entry : input.package->entries) {
            if (!entry.lz4Compressed) {
                continue;
            }
            Progress progress{Phase::checkingImages, planner::toString(input.slot),
                              entry.name, 0, entry.size, 0, 0};
            report(observer, progress);
            auto size = firmware::validateLz4(
                *input.package, entry,
                [&](std::uint64_t done, std::uint64_t all) {
                    progress.fileDone = done;
                    progress.fileTotal = all;
                    report(observer, progress);
                },
                cancel);
            if (!size) {
                return std::move(size).error();
            }
            lz4Sizes[{input.package.get(), entry.name}] = *size;
        }
    }
    const auto imageSize = [&lz4Sizes](const planner::PlannedFile& file) {
        if (!file.entry.lz4Compressed) {
            return file.entry.size;
        }
        return lz4Sizes.at({file.package.get(), file.entry.name});
    };
    if (cancel != nullptr && cancel->honoured(false)) {
        return makeError(ErrorCode::cancelled, "Flash cancelled before connecting");
    }

    odin::Session session(interface, options.timeouts);
    report(observer, {Phase::connecting, {}, {}, 0, 0, 0, 0});
    auto info = session.open(/*resume=*/false);
    if (!info) {
        return std::move(info).error();
    }
    if (observer.onSession) {
        observer.onSession(*info);
    }
    log(observer, LogLevel::info,
        "Session open: protocol " + std::to_string(info->protocolVersion) +
            ", part size " + std::to_string(info->packetSize) + " bytes");

    report(observer, {Phase::readingPit, {}, {}, 0, 0, 0, 0});
    auto pitBytes = session.readPit();
    if (!pitBytes) {
        return std::move(pitBytes).error(); // session state uncertain; left open
    }
    auto devicePit = pit::parse(pitBytes->data(), pitBytes->size());
    if (!devicePit) {
        return abortCleanly(session, std::move(devicePit).error());
    }

    report(observer, {Phase::planning, {}, {}, 0, 0, 0, 0});
    auto plan = planner::buildPlan(*devicePit, request.packages);
    if (observer.onPlan) {
        observer.onPlan(plan);
    }
    if (!plan.flashable()) {
        return abortCleanly(session,
                            makeError(ErrorCode::planRefused,
                                      plan.refusals.empty()
                                          ? std::string("No images map to this device")
                                          : "Flash refused: " + plan.refusals.front()));
    }
    if (request.reviewed && !planner::samePlan(plan, *request.reviewed)) {
        return abortCleanly(
            session, makeError(ErrorCode::planRefused,
                               "The device's PIT gives a different plan from the one "
                               "you reviewed; review again"));
    }
    if (auto confirmed = planner::checkConfirmation(plan, request.confirmation);
        !confirmed) {
        return abortCleanly(session, confirmed.error());
    }
    if (auto unchanged = verifyFilesUnchanged(request.packages); !unchanged) {
        return abortCleanly(session, unchanged.error());
    }
    if (cancel != nullptr && cancel->honoured(false)) {
        return abortCleanly(session, makeError(ErrorCode::cancelled,
                                               "Flash cancelled before any data was sent"));
    }

    std::uint64_t total = 0;
    for (const auto& file : plan.files) {
        const auto size = imageSize(file);
        if (size > std::numeric_limits<std::uint64_t>::max() - total) {
            return abortCleanly(session,
                                makeError(ErrorCode::invalidArgument, "Total size overflows"));
        }
        total += size;
    }
    if (auto announced = session.announceTotalBytes(total); !announced) {
        return announced.error();
    }

    std::uint64_t completed = 0;
    for (const auto& file : plan.files) {
        const auto size = imageSize(file);
        std::unique_ptr<firmware::Lz4StreamReader> lz4;
        if (file.entry.lz4Compressed) {
            auto opened = firmware::Lz4StreamReader::open(file.package, file.entry, size);
            if (!opened) {
                return session.dataSent() ? std::move(opened).error()
                                          : abortCleanly(session, std::move(opened).error());
            }
            lz4 = std::move(*opened);
        }
        const odin::ImageReader reader =
            [&file, &lz4](std::uint64_t offset, std::uint8_t* destination,
                          std::size_t count) -> Status {
            if (lz4) {
                return lz4->read(offset, destination, count);
            }
            return firmware::readEntryBytes(*file.package, file.entry, offset,
                                            destination, count);
        };

        Progress progress{Phase::transferring, planner::toString(file.slot),
                          file.entry.name, 0, size, completed, total};
        report(observer, progress);
        log(observer, LogLevel::info,
            std::string("Flashing ") + planner::toString(file.slot) + ": " +
                file.entry.name + " -> " + file.partition.partitionName);
        auto transferred = session.transferFile(
            {file.partition.identifier, file.partition.binaryType,
             file.partition.deviceType},
            size, reader,
            [&](std::uint64_t done, bool committing) {
                progress.phase = committing ? Phase::committing : Phase::transferring;
                progress.fileDone = done;
                progress.totalDone = completed + done;
                report(observer, progress);
            },
            cancel);
        if (!transferred) {
            Error error = transferred.error();
            if (!session.dataSent()) {
                return abortCleanly(session, std::move(error));
            }
            error.message = "Stopped while flashing " + file.entry.name + " to " +
                            file.partition.partitionName + ": " + error.message;
            return error;
        }
        completed += size;
    }

    report(observer, {Phase::closing, {}, {}, 0, 0, completed, total});
    if (auto closed = session.close(options.reboot); !closed) {
        Error error = closed.error();
        error.message = "Every image was written and acknowledged. " + error.message;
        return error;
    }
    if (session.rebootUnconfirmed()) {
        log(observer, LogLevel::warning,
            "The phone disconnected right after the reboot request instead of replying; "
            "it is restarting");
    }
    report(observer, {Phase::done, {}, {}, 0, 0, completed, total});
    return Report{*info, std::move(plan), completed, options.reboot};
}

} // namespace bifrost::flash
