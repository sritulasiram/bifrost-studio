#include "support/sim_device.hpp"

#include "bifrost/protocol/packet.hpp"
#include "bifrost/transport/descriptors.hpp"

#include <algorithm>
#include <cstring>

namespace sim {
namespace {

using bifrost::ErrorCode;
using bifrost::makeError;
using bifrost::odin::kControlPacketSize;
using bifrost::odin::readLittleEndian32;
using bifrost::odin::writeLittleEndian32;

constexpr std::uint32_t kFail = 0xFFFFFFFF;
constexpr std::uint32_t kLegacyPacketSize = 131072;
constexpr std::uint32_t kLegacyParts = 240;
constexpr std::uint32_t kModernPacketSize = 1048576;
constexpr std::uint32_t kModernParts = 30;
constexpr std::size_t kPitChunk = 500;

std::string hex(std::uint32_t value)
{
    char text[16];
    std::snprintf(text, sizeof text, "0x%x", value);
    return text;
}

} // namespace

Device::Device(Config config) : config_(std::move(config))
{
    if (!config_.pit.empty()) {
        auto parsed = bifrost::pit::parse(config_.pit.data(), config_.pit.size());
        if (parsed) {
            table_ = std::move(*parsed);
        }
    }
}

const bifrost::transport::OdinInterface& Device::selection() const noexcept
{
    return selection_;
}

void Device::violation(const std::string& message)
{
    violations_.push_back(message);
}

void Device::respond(std::uint32_t id, std::uint32_t value, unsigned int delayMs)
{
    Bytes bytes(8);
    writeLittleEndian32(bytes.data(), id);
    writeLittleEndian32(bytes.data() + 4, value);
    reads_.push_back({std::move(bytes), delayMs});
}

void Device::respondRaw(Bytes bytes)
{
    reads_.push_back({std::move(bytes), 0});
}

std::uint32_t Device::statusFor(Point point)
{
    const auto occurrence = pointCounts_[point]++;
    const auto& rule = config_.faults.negativeStatus;
    if (rule && rule->point == point && rule->occurrence == occurrence) {
        return static_cast<std::uint32_t>(rule->status);
    }
    return 0;
}

bool Device::sent(std::uint32_t command, std::uint32_t request) const
{
    return std::any_of(commands_.begin(), commands_.end(), [&](const Command& c) {
        return c.command == command && c.request == request;
    });
}

const Bytes* Device::written(std::uint32_t partitionId) const
{
    const auto found = partitions_.find(partitionId);
    return found == partitions_.end() ? nullptr : &found->second.bytes;
}

bool Device::sessionLeftOpen() const
{
    switch (phase_) {
    case Phase::awaitingPartSize:
    case Phase::open:
    case Phase::inFile:
    case Phase::receivingParts:
    case Phase::awaitingSequenceEnd:
        return true;
    default:
        return false;
    }
}

void Device::reenterDownloadMode()
{
    phase_ = Phase::awaitingHandshake;
    handshaken_ = false;
    reads_.clear();
    held_.clear();
    packetSize_ = 0;
    partsPerSequence_ = 0;
    sessionEnded_ = false;
    rebooted_ = false;
}

bifrost::Expected<std::size_t> Device::bulkWrite(const std::uint8_t* data,
                                                 std::size_t size, unsigned int)
{
    if (disconnected_) {
        return makeError(disconnectCode_, "simulated device disconnected");
    }
    if (lateZlpPending_) {
        // The zero-length packet arrives after the host's drain window.
        reads_.push_back({{}, 0});
        lateZlpPending_ = false;
    }
    switch (phase_) {
    case Phase::awaitingHandshake:
        if (size == 4 && std::memcmp(data, "ODIN", 4) == 0) {
            respondRaw({'L', 'O', 'K', 'E'});
            phase_ = Phase::awaitingBegin;
            handshaken_ = true;
        } else {
            violation("expected the ODIN handshake, got " + std::to_string(size) +
                      " bytes");
        }
        return size;
    case Phase::receivingParts:
        return handlePart(data, size);
    default:
        break;
    }
    if (phase_ == Phase::awaitingBegin && handshaken_ && size == 4 &&
        std::memcmp(data, "ODIN", 4) == 0) {
        // Observed: a repeated handshake in the same Download Mode entry gets
        // no reply (T6). Not a host bug by itself; the host must use resume.
        ++repeatedHandshakes_;
        return size;
    }
    if (size != kControlPacketSize) {
        violation("unexpected " + std::to_string(size) + "-byte write outside a sequence");
        respond(kFail, 0);
        return size;
    }
    const bool holdUntilZlp = config_.requireZlpAfterControl && packetSize_ == kModernPacketSize;
    const auto queued = reads_.size();
    auto handled = handleControl(data);
    if (holdUntilZlp) {
        for (auto i = reads_.begin() + static_cast<std::ptrdiff_t>(queued); i != reads_.end(); ++i) {
            held_.push_back(std::move(*i));
        }
        reads_.erase(reads_.begin() + static_cast<std::ptrdiff_t>(queued), reads_.end());
    }
    return handled;
}

bifrost::Expected<std::size_t> Device::handleControl(const std::uint8_t* packet)
{
    const auto command = readLittleEndian32(packet);
    const auto request = readLittleEndian32(packet + 4);
    const auto arg0 = readLittleEndian32(packet + 8);
    const auto arg1 = readLittleEndian32(packet + 12);
    commands_.push_back({command, request});
    const auto name = hex(command) + "/" + std::to_string(request);

    const auto reject = [&](const std::string& why) {
        violation(name + ": " + why);
        respond(kFail, 0);
        return bifrost::Expected<std::size_t>(kControlPacketSize);
    };
    const bool sessionIdle = phase_ == Phase::open;

    switch (command) {
    case 0x64:
        if (request == 0) {
            if (phase_ != Phase::awaitingBegin) {
                return reject("begin-session outside the handshake state");
            }
            beginArgument_ = arg0;
            announcedTotal_.reset(); // per-session accounting
            committedBytes_ = 0;
            if (config_.faults.rejectBeginSession) {
                respond(kFail, 0);
                return kControlPacketSize;
            }
            std::uint32_t ack = static_cast<std::uint32_t>(config_.protocolVersion & 0x7FFF)
                                << 16;
            if (config_.advertiseCompression) {
                ack |= 0x80000000u;
            }
            respond(0x64, ack);
            if (config_.protocolVersion > 1) {
                phase_ = Phase::awaitingPartSize;
            } else {
                packetSize_ = kLegacyPacketSize;
                partsPerSequence_ = kLegacyParts;
                phase_ = Phase::open;
            }
            return kControlPacketSize;
        }
        if (request == 5) {
            if (phase_ != Phase::awaitingPartSize) {
                return reject("part size sent to a protocol v0/1 device or twice");
            }
            if (arg0 != kModernPacketSize) {
                violation(name + ": unexpected part size " + std::to_string(arg0));
            }
            packetSize_ = arg0;
            partsPerSequence_ = kModernParts;
            phase_ = Phase::open;
            respond(0x64, statusFor(Point::partSize));
            return kControlPacketSize;
        }
        if (request == 1) {
            violation("0x64/1 must never be sent (docs/requirements.md, S-4)");
            respond(0x64, 0);
            return kControlPacketSize;
        }
        if (request == 2) {
            if (!sessionIdle) {
                return reject("total bytes outside an idle open session");
            }
            announcedTotal_ = static_cast<std::uint64_t>(arg0) |
                              (static_cast<std::uint64_t>(arg1) << 32);
            respond(0x64, statusFor(Point::totalBytes));
            return kControlPacketSize;
        }
        return reject("unknown session request");

    case 0x65: {
        if (!sessionIdle) {
            return reject("PIT request outside an idle open session");
        }
        const auto& pit = config_.pit;
        if (request == 1) {
            respond(0x65, static_cast<std::uint32_t>(pit.size()));
            return kControlPacketSize;
        }
        if (request == 2) {
            const auto offset = static_cast<std::size_t>(arg0) * kPitChunk;
            if (offset >= pit.size()) {
                return reject("PIT part index out of range");
            }
            const auto end = std::min(pit.size(), offset + kPitChunk);
            respondRaw(Bytes(pit.begin() + static_cast<std::ptrdiff_t>(offset),
                             pit.begin() + static_cast<std::ptrdiff_t>(end)));
            return kControlPacketSize;
        }
        if (request == 3) {
            respond(0x65, 0);
            return kControlPacketSize;
        }
        return reject("unknown PIT request");
    }

    case 0x66:
        if (request == 0) {
            if (!sessionIdle) {
                return reject("file transfer started while another is unfinished");
            }
            const auto status = statusFor(Point::fileStart);
            filePartition_.reset();
            if (static_cast<std::int32_t>(status) >= 0) {
                phase_ = Phase::inFile;
            }
            respond(0x66, status);
            return kControlPacketSize;
        }
        if (request == 2) {
            if (phase_ != Phase::inFile) {
                return reject("sequence started outside a file transfer");
            }
            if (arg0 == 0 || arg0 % packetSize_ != 0) {
                return reject("sequence size " + std::to_string(arg0) +
                              " is not a whole number of parts");
            }
            expectedParts_ = arg0 / packetSize_;
            if (expectedParts_ > partsPerSequence_) {
                violation(name + ": " + std::to_string(expectedParts_) +
                          " parts exceeds the sequence limit");
            }
            paddedSize_ = arg0;
            partIndex_ = 0;
            sequence_.clear();
            const auto status = statusFor(Point::sequenceStart);
            if (static_cast<std::int32_t>(status) >= 0) {
                phase_ = Phase::receivingParts;
            }
            respond(0x66, status);
            return kControlPacketSize;
        }
        if (request == 3) {
            if (phase_ != Phase::awaitingSequenceEnd) {
                return reject("end of sequence before all parts were received");
            }
            handleSequenceEnd(packet);
            return kControlPacketSize;
        }
        return reject("unknown file-transfer request");

    case 0x67:
        if (request == 0) {
            if (!sessionIdle) {
                return reject("session ended in the middle of a file");
            }
            if (announcedTotal_ && committedBytes_ != 0 &&
                committedBytes_ != *announcedTotal_) {
                violation("announced " + std::to_string(*announcedTotal_) +
                          " bytes but " + std::to_string(committedBytes_) +
                          " were committed");
            }
            sessionEnded_ = true;
            // The bootloader stays in Download Mode and accepts a new session,
            // but without a second handshake (SM-N920I, T6).
            phase_ = Phase::awaitingBegin;
            respond(0x67, statusFor(Point::endSession));
            return kControlPacketSize;
        }
        if (request == 1) {
            if (!sessionEnded_) {
                return reject("reboot before the session was ended");
            }
            rebooted_ = true;
            phase_ = Phase::ended;
            if (config_.faults.rebootDropsOff) {
                disconnected_ = true;
                disconnectCode_ = *config_.faults.rebootDropsOff;
                return kControlPacketSize;
            }
            if (config_.faults.rebootSilent) {
                return kControlPacketSize;
            }
            respond(0x67, statusFor(Point::reboot));
            return kControlPacketSize;
        }
        return reject("unknown end-session request");

    default:
        return reject("unknown command");
    }
}

bifrost::Expected<std::size_t> Device::handlePart(const std::uint8_t* data,
                                                  std::size_t size)
{
    auto& faults = config_.faults;
    if (size != packetSize_) {
        violation("data part of " + std::to_string(size) + " bytes; expected " +
                  std::to_string(packetSize_));
        respond(kFail, 0);
        return size;
    }
    if (config_.onPart) {
        config_.onPart(partsTotal_);
    }
    if (faults.stallAtPart && *faults.stallAtPart == partsTotal_) {
        faults.stallAtPart.reset();
        return makeError(ErrorCode::usbStall, "simulated endpoint stall");
    }
    if (faults.disconnectAfterBytes && dataBytes_ + size > *faults.disconnectAfterBytes) {
        disconnected_ = true;
        return makeError(ErrorCode::usbDisconnected, "simulated cable pull");
    }
    sequence_.insert(sequence_.end(), data, data + size);
    dataBytes_ += size;

    if (faults.shortAckAtPart && *faults.shortAckAtPart == partsTotal_) {
        respondRaw({0, 0, 0, 0});
    } else {
        const bool wrong = faults.wrongAckAtPart && *faults.wrongAckAtPart == partsTotal_;
        respond(0x00, wrong ? partIndex_ + 1 : partIndex_);
    }
    ++partIndex_;
    ++partsTotal_;
    if (partIndex_ == expectedParts_) {
        phase_ = Phase::awaitingSequenceEnd;
    }
    return size;
}

void Device::handleSequenceEnd(const std::uint8_t* packet)
{
    const auto destination = readLittleEndian32(packet + 8);
    const auto realSize = readLittleEndian32(packet + 12);
    const auto binaryType = readLittleEndian32(packet + 16);
    const auto deviceType = readLittleEndian32(packet + 20);
    const auto identifier = readLittleEndian32(packet + 24);
    const auto endOfFile = readLittleEndian32(packet + 28);

    if (destination != 0) {
        violation("end of sequence with destination " + std::to_string(destination));
    }
    if (realSize == 0 || realSize > paddedSize_ || realSize + packetSize_ <= paddedSize_) {
        violation("real size " + std::to_string(realSize) + " does not fit padded size " +
                  std::to_string(paddedSize_));
    }
    if (binaryType != 0) {
        violation("modem (binary type 1) end-of-sequence must not be sent");
    }
    if (endOfFile > 1) {
        violation("end-of-file flag " + std::to_string(endOfFile));
    }
    const bifrost::pit::Entry* entry =
        table_ ? table_->findByIdentifier(identifier) : nullptr;
    if (entry == nullptr) {
        violation("partition " + std::to_string(identifier) + " is not in the PIT");
    } else if (entry->binaryType != binaryType || entry->deviceType != deviceType) {
        violation("partition " + std::to_string(identifier) +
                  " binary/device type does not match the PIT");
    }
    if (filePartition_ && *filePartition_ != identifier) {
        violation("partition changed in the middle of a file");
    }
    auto& partition = partitions_[identifier];
    if (!filePartition_ && partition.complete) {
        violation("partition " + std::to_string(identifier) + " written twice");
    }
    const auto keep = std::min<std::size_t>(realSize, sequence_.size());
    if (std::any_of(sequence_.begin() + static_cast<std::ptrdiff_t>(keep), sequence_.end(),
                    [](std::uint8_t b) { return b != 0; })) {
        violation("sequence padding is not zero-filled");
    }
    partition.bytes.insert(partition.bytes.end(), sequence_.begin(),
                           sequence_.begin() + static_cast<std::ptrdiff_t>(keep));
    partition.binaryType = binaryType;
    partition.deviceType = deviceType;
    ++partition.sequences;
    committedBytes_ += keep;

    const auto status = statusFor(Point::sequenceEnd);
    respond(0x66, status, config_.faults.commitDelayMs.value_or(0));
    if (config_.faults.lateZlpAfterSequence &&
        *config_.faults.lateZlpAfterSequence == sequencesTotal_) {
        lateZlpPending_ = true;
    }
    ++sequencesTotal_;

    if (endOfFile == 1) {
        partition.complete = true;
        filePartition_.reset();
        phase_ = Phase::open;
    } else {
        filePartition_ = identifier;
        phase_ = Phase::inFile;
    }
}

bifrost::Expected<std::size_t> Device::bulkRead(std::uint8_t* data, std::size_t size,
                                                unsigned int timeoutMs)
{
    if (disconnected_) {
        return makeError(disconnectCode_, "simulated device disconnected");
    }
    if (reads_.empty()) {
        return makeError(ErrorCode::usbTimeout, "simulated read timeout");
    }
    if (reads_.front().delayMs > timeoutMs) {
        // The response is still on its way; it stays queued.
        return makeError(ErrorCode::usbTimeout, "simulated slow response");
    }
    auto next = std::move(reads_.front());
    reads_.pop_front();
    if (next.bytes.size() > size) {
        return makeError(ErrorCode::usbOther, "simulated overflow");
    }
    std::copy(next.bytes.begin(), next.bytes.end(), data);
    return next.bytes.size();
}

bifrost::Status Device::sendZeroLength(unsigned int)
{
    if (disconnected_) {
        return makeError(disconnectCode_, "simulated device disconnected");
    }
    ++zlpOut_;
    // The packet still reached the device; only the host saw an error.
    while (!held_.empty()) {
        reads_.push_back(std::move(held_.front()));
        held_.pop_front();
    }
    if (config_.faults.zlpOutFailure && (!config_.faults.zlpFailureAfterData || partsTotal_ > 0)) {
        if (*config_.faults.zlpOutFailure == ErrorCode::usbDisconnected) {
            disconnected_ = true;
        }
        return makeError(*config_.faults.zlpOutFailure, "simulated ZLP failure");
    }
    return bifrost::success();
}

bifrost::Status Device::drainZeroLength(unsigned int)
{
    if (disconnected_) {
        return makeError(disconnectCode_, "simulated device disconnected");
    }
    if (!reads_.empty() && reads_.front().bytes.empty() && reads_.front().delayMs == 0) {
        reads_.pop_front();
    }
    return bifrost::success();
}

bifrost::Status Device::release()
{
    released_ = true;
    return bifrost::success();
}

// ---- Transport ---------------------------------------------------------------

namespace {

class Proxy final : public bifrost::transport::ClaimedInterface {
public:
    explicit Proxy(std::shared_ptr<Device> device) : device_(std::move(device)) {}
    const bifrost::transport::OdinInterface& selection() const noexcept override
    {
        return device_->selection();
    }
    bifrost::Expected<std::size_t> bulkWrite(const std::uint8_t* data, std::size_t size,
                                             unsigned int timeoutMs) override
    {
        return device_->bulkWrite(data, size, timeoutMs);
    }
    bifrost::Expected<std::size_t> bulkRead(std::uint8_t* data, std::size_t size,
                                            unsigned int timeoutMs) override
    {
        return device_->bulkRead(data, size, timeoutMs);
    }
    bifrost::Status sendZeroLength(unsigned int timeoutMs) override
    {
        return device_->sendZeroLength(timeoutMs);
    }
    bifrost::Status drainZeroLength(unsigned int timeoutMs) override
    {
        return device_->drainZeroLength(timeoutMs);
    }
    bifrost::Status release() override { return device_->release(); }

private:
    std::shared_ptr<Device> device_;
};

} // namespace

Transport::Transport(std::shared_ptr<Device> device) : device_(std::move(device)) {}

bifrost::transport::UsbIdentity Transport::identity()
{
    return {bifrost::transport::kSamsungVendorId, 0x685D, 1, 5};
}

bifrost::Expected<bifrost::transport::UsbEnumeration> Transport::enumerate()
{
    using namespace bifrost::transport;
    UsbEnumeration enumeration;
    std::lock_guard<std::mutex> lock(mutex_);
    if (!attached_) {
        return enumeration;
    }
    AlternateSetting data;
    data.interfaceNumber = 1;
    data.interfaceClass = 0x0A;
    data.endpoints = {{0x81, TransferType::bulk, 512}, {0x01, TransferType::bulk, 512}};
    UsbDeviceInfo info;
    info.identity = identity();
    info.alternates = {data};
    info.odinInterface = findOdinInterface(info.alternates);
    enumeration.devices.push_back(std::move(info));
    return enumeration;
}

bifrost::Expected<std::unique_ptr<bifrost::transport::ClaimedInterface>> Transport::claim(
    const bifrost::transport::UsbIdentity& wanted)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (!attached_ || !(wanted == identity())) {
        return makeError(ErrorCode::usbDisconnected, "simulated device not present");
    }
    ++claims_;
    return std::unique_ptr<bifrost::transport::ClaimedInterface>(
        std::make_unique<Proxy>(device_));
}

bool Transport::startHotplug(bifrost::transport::HotplugCallback callback)
{
    std::lock_guard<std::mutex> lock(mutex_);
    hotplug_ = std::move(callback);
    return true;
}

void Transport::stopHotplug()
{
    std::lock_guard<std::mutex> lock(mutex_);
    hotplug_ = nullptr;
}

void Transport::setAttached(bool attached)
{
    bifrost::transport::HotplugCallback callback;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        attached_ = attached;
        callback = hotplug_;
    }
    if (callback) {
        callback();
    }
}

} // namespace sim
