// A stateful simulation of a Samsung Download Mode bootloader, for tests.
//
// It implements the Odin protocol as Bifrost Studio understands it from the
// reference sources (docs/protocol.md). It is NOT evidence of how real
// bootloaders behave; it checks that the host side is internally consistent,
// that every byte of every image reaches the right partition, and that the
// host reacts safely to injected faults.
#ifndef BIFROST_TEST_SIM_DEVICE_HPP
#define BIFROST_TEST_SIM_DEVICE_HPP

#include "bifrost/common/error.hpp"
#include "bifrost/pit/pit.hpp"
#include "bifrost/transport/transport.hpp"

#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace sim {

using Bytes = std::vector<std::uint8_t>;

// Points at which a negative status can be injected into a response.
enum class Point {
    partSize,      // 0x64/5
    totalBytes,    // 0x64/2
    fileStart,     // 0x66/0
    sequenceStart, // 0x66/2
    sequenceEnd,   // 0x66/3
    endSession,    // 0x67/0
    reboot,        // 0x67/1
};

struct NegativeStatus {
    Point point{Point::sequenceEnd};
    std::uint64_t occurrence{}; // 0-based count of that point
    std::int32_t status{-4};
};

// Rule-based faults. Part indices count every data part in the session
// (0-based); sequence indices count every end-of-sequence packet.
struct Faults {
    std::optional<std::uint64_t> wrongAckAtPart;
    std::optional<std::uint64_t> shortAckAtPart;
    std::optional<std::uint64_t> stallAtPart;
    // The cable is pulled once this many data-part bytes have been accepted.
    std::optional<std::uint64_t> disconnectAfterBytes;
    std::optional<NegativeStatus> negativeStatus;
    // A zero-length IN packet arrives after the drain window following this
    // sequence, so the next response read sees a 0-byte transfer.
    std::optional<std::uint64_t> lateZlpAfterSequence;
    // End-of-sequence responses take this long; a shorter read timeout times out.
    std::optional<unsigned int> commitDelayMs;
    // Every zero-length OUT transfer fails with this code (only once data
    // parts have been sent, when zlpFailureAfterData is set).
    std::optional<bifrost::ErrorCode> zlpOutFailure;
    bool zlpFailureAfterData{};
    // BOOTLOADER_FAIL (0xFFFFFFFF) in reply to the begin-session request.
    bool rejectBeginSession{};
    // The phone restarts on the reboot request without replying: it leaves the
    // bus and every later transfer fails with this code (SM-N920I showed
    // LIBUSB_ERROR_OTHER, i.e. usbOther, 2026-11-08).
    std::optional<bifrost::ErrorCode> rebootDropsOff;
    // The phone never answers the reboot request but stays attached.
    bool rebootSilent{};
};

struct Config {
    Bytes pit;
    std::uint16_t protocolVersion{2};
    bool advertiseCompression{};
    Faults faults;
    // Models the SM-N920I (2026-11-08): once the part size is set (0x64/5),
    // a control packet is only answered after a zero-length OUT packet ends
    // the 1024-byte transfer.
    bool requireZlpAfterControl{true};
    // Called (on the caller's thread) before each data part is accepted, with
    // the session-wide part index. Tests use it to hold a transfer open.
    std::function<void(std::uint64_t part)> onPart;
};

struct PartitionWrite {
    Bytes bytes;
    bool complete{};       // an end-of-file sequence was committed
    std::uint32_t binaryType{};
    std::uint32_t deviceType{};
    std::uint32_t sequences{};
};

struct Command {
    std::uint32_t command{};
    std::uint32_t request{};
};

class Device final : public bifrost::transport::ClaimedInterface {
public:
    explicit Device(Config config);

    // ClaimedInterface
    const bifrost::transport::OdinInterface& selection() const noexcept override;
    bifrost::Expected<std::size_t> bulkWrite(const std::uint8_t* data, std::size_t size,
                                             unsigned int timeoutMs) override;
    bifrost::Expected<std::size_t> bulkRead(std::uint8_t* data, std::size_t size,
                                            unsigned int timeoutMs) override;
    bifrost::Status sendZeroLength(unsigned int timeoutMs) override;
    bifrost::Status drainZeroLength(unsigned int timeoutMs) override;
    bifrost::Status release() override;

    // Observations.
    const std::map<std::uint32_t, PartitionWrite>& partitions() const { return partitions_; }
    const Bytes* written(std::uint32_t partitionId) const;
    const std::vector<std::string>& violations() const { return violations_; }
    const std::vector<Command>& commands() const { return commands_; }
    bool sent(std::uint32_t command, std::uint32_t request) const;
    std::optional<std::uint32_t> beginArgument() const { return beginArgument_; }
    std::optional<std::uint64_t> announcedTotal() const { return announcedTotal_; }
    std::uint64_t dataBytesReceived() const { return dataBytes_; }
    std::uint64_t partsReceived() const { return partsTotal_; }
    std::uint64_t zeroLengthOutCount() const { return zlpOut_; }
    bool sessionEnded() const { return sessionEnded_; }
    bool rebooted() const { return rebooted_; }
    bool disconnected() const { return disconnected_; }
    bool released() const { return released_; }
    // True when the device would still be waiting in an open session.
    bool sessionLeftOpen() const;
    // The user restarts the phone into Download Mode: it expects a fresh
    // handshake again. Written partitions are kept.
    void reenterDownloadMode();

private:
    enum class Phase {
        awaitingHandshake,
        awaitingBegin,
        awaitingPartSize,
        open,
        inFile,          // after 0x66/0, between sequences
        receivingParts,
        awaitingSequenceEnd,
        ended,
    };

    struct Response {
        Bytes bytes;
        unsigned int delayMs{};
    };

    void violation(const std::string& message);
    void respond(std::uint32_t id, std::uint32_t value, unsigned int delayMs = 0);
    void respondRaw(Bytes bytes);
    std::uint32_t statusFor(Point point);
    bifrost::Expected<std::size_t> handleControl(const std::uint8_t* packet);
    bifrost::Expected<std::size_t> handlePart(const std::uint8_t* data, std::size_t size);
    void handleSequenceEnd(const std::uint8_t* packet);

    Config config_;
    std::optional<bifrost::pit::Table> table_;
    bifrost::transport::OdinInterface selection_{1, 0, 0x81, 0x01};
    Phase phase_{Phase::awaitingHandshake};
    std::deque<Response> reads_;
    std::deque<Response> held_; // responses waiting for the terminating ZLP

    std::uint32_t packetSize_{};
    std::uint32_t partsPerSequence_{};
    std::uint32_t expectedParts_{};
    std::uint32_t paddedSize_{};
    std::uint32_t partIndex_{};
    Bytes sequence_;
    std::optional<std::uint32_t> filePartition_;

    std::map<std::uint32_t, PartitionWrite> partitions_;
    std::vector<std::string> violations_;
    std::vector<Command> commands_;
    std::map<Point, std::uint64_t> pointCounts_;
    std::optional<std::uint32_t> beginArgument_;
    std::optional<std::uint64_t> announcedTotal_;
    std::uint64_t dataBytes_{};
    std::uint64_t committedBytes_{};
    std::uint64_t partsTotal_{};
    std::uint64_t sequencesTotal_{};
    std::uint64_t zlpOut_{};
    bool lateZlpPending_{};
    bool sessionEnded_{};
    bool rebooted_{};
    bool disconnected_{};
    bifrost::ErrorCode disconnectCode_{bifrost::ErrorCode::usbDisconnected};
    bool released_{};
    bool handshaken_{};
    std::uint64_t repeatedHandshakes_{};

public:
    std::uint64_t repeatedHandshakes() const { return repeatedHandshakes_; }
};

// A UsbTransport exposing one simulated device. claim() hands out a proxy
// to the Device, which the test owns and inspects afterwards.
class Transport final : public bifrost::transport::UsbTransport {
public:
    explicit Transport(std::shared_ptr<Device> device);

    static bifrost::transport::UsbIdentity identity();

    bifrost::Expected<bifrost::transport::UsbEnumeration> enumerate() override;
    bifrost::Expected<std::unique_ptr<bifrost::transport::ClaimedInterface>> claim(
        const bifrost::transport::UsbIdentity& identity) override;
    bool startHotplug(bifrost::transport::HotplugCallback callback) override;
    void stopHotplug() override;

    // Simulates plugging or unplugging the device and fires hot-plug.
    void setAttached(bool attached);
    std::size_t claims() const { return claims_; }

private:
    std::shared_ptr<Device> device_;
    std::mutex mutex_;
    bifrost::transport::HotplugCallback hotplug_;
    bool attached_{true};
    std::size_t claims_{};
};

} // namespace sim

#endif
