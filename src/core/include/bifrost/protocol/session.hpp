#ifndef BIFROST_PROTOCOL_SESSION_HPP
#define BIFROST_PROTOCOL_SESSION_HPP

#include "bifrost/common/cancel.hpp"
#include "bifrost/common/error.hpp"
#include "bifrost/pit/pit.hpp"
#include "bifrost/transport/transport.hpp"

#include <cstdint>
#include <functional>
#include <vector>

namespace bifrost::odin {

struct SessionInfo {
    std::uint32_t sessionAck{};      // raw begin-session response value
    std::uint16_t protocolVersion{}; // (ack >> 16) & 0x7FFF, as odin4/Thor
    bool supportsCompressedTransfer{};
    std::uint32_t packetSize{};      // bytes per data part
    std::uint32_t partsPerSequence{};
};

struct Timeouts {
    unsigned int handshakeMs{3000};
    unsigned int controlMs{10000};
    // Long, like the commit wait: a timeout mid-partition guarantees a
    // partial write (SM-N920I system.img write timed out at 60 s, 2026-11-08).
    unsigned int partMs{120000};
    unsigned int sequenceEndMs{120000};
    unsigned int zeroLengthMs{100};
};

struct PartitionTarget {
    std::uint32_t identifier{};
    std::uint32_t binaryType{};
    std::uint32_t deviceType{};
};

// Reads `size` bytes of the image at `offset` into `destination`.
using ImageReader =
    std::function<Status(std::uint64_t offset, std::uint8_t* destination,
                         std::size_t size)>;

// Called after every acknowledged part (committing == false) and before
// waiting for each end-of-sequence acknowledgement (committing == true).
using TransferProgress =
    std::function<void(std::uint64_t acknowledgedBytes, bool committing)>;

// One Odin session, as an explicit state machine:
//
//   idle --open()--> open --close()--> closed
//     |                |  readPit / announceTotalBytes / transferFile
//     +-- any failure -+--> failed (terminal)
//
// Operations called in the wrong state fail with illegalState and send
// nothing. dataSent() reports whether any partition byte left the host.
class Session {
public:
    enum class State { idle, open, closed, failed };

    explicit Session(transport::ClaimedInterface& interface, Timeouts timeouts = {});

    // `resume` skips the ODIN/LOKE handshake: the bootloader performs it once
    // per entry into Download Mode, so a session after one that ended without
    // a reboot must resume (Heimdall --resume, BridgeManager.cpp:498-502;
    // SM-N920I hardware test T6, 2026-11-08).
    Expected<SessionInfo> open(bool resume = false);
    Expected<std::vector<std::uint8_t>> readPit();
    Status announceTotalBytes(std::uint64_t totalBytes);
    Status transferFile(const PartitionTarget& target, std::uint64_t size,
                        const ImageReader& reader,
                        const TransferProgress& progress,
                        const CancelToken* cancel);
    // Ends the session (0x67/0), then optionally requests a reboot (0x67/1).
    Status close(bool reboot);

    State state() const noexcept { return state_; }
    bool dataSent() const noexcept { return dataSent_; }
    // close(true) succeeded because the phone left the bus right after the
    // reboot request was sent, without replying (it was restarting).
    bool rebootUnconfirmed() const noexcept { return rebootUnconfirmed_; }
    const SessionInfo& info() const noexcept { return info_; }

private:
    // File transfer (src/core/src/protocol/file_transfer.cpp).
    struct TransferCursor;
    Status sendSequence(const PartitionTarget& target, std::uint32_t realSize, bool last,
                        const ImageReader& reader, const TransferProgress& progress,
                        std::vector<std::uint8_t>& buffer, TransferCursor& cursor);
    Status sendPart(std::uint32_t index, std::uint32_t parts, const ImageReader& reader,
                    std::vector<std::uint8_t>& buffer, TransferCursor& cursor);

    Status requireState(State expected, const char* operation) const;
    Status handshake();
    Expected<SessionInfo> beginSession();
    Error fail(Error error);
    Status writeAll(const std::uint8_t* data, std::size_t size,
                    unsigned int timeoutMs);
    // A 1024-byte control packet followed by a best-effort zero-length packet.
    Status writeControl(const std::uint8_t* packet, unsigned int timeoutMs);
    Expected<std::uint32_t> exchange(std::uint32_t command, std::uint32_t request,
                                     std::initializer_list<std::uint32_t> args,
                                     std::uint32_t expectedResponse,
                                     unsigned int timeoutMs);
    Expected<std::uint32_t> exchangePacket(const std::uint8_t* packet,
                                           std::uint32_t expectedResponse,
                                           unsigned int timeoutMs);
    Status bestEffortZeroLengthOut(unsigned int timeoutMs = 0);
    Status bestEffortDrain();

    transport::ClaimedInterface& interface_;
    Timeouts timeouts_;
    State state_{State::idle};
    bool dataSent_{};
    bool rebootUnconfirmed_{};
    SessionInfo info_{};
};

} // namespace bifrost::odin

#endif
