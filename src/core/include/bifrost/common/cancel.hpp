#ifndef BIFROST_COMMON_CANCEL_HPP
#define BIFROST_COMMON_CANCEL_HPP

#include <atomic>
#include <cstdint>

namespace bifrost {

// Cancellation is a request, honoured only at safe points:
//  - beforeData: honoured any time before the first partition byte is sent;
//    ignored once data transfer has begun.
//  - afterSequence: additionally honoured at the next sequence boundary
//    during transfer. This leaves the device partially written and must only
//    be requested after the user explicitly accepts that.
enum class CancelLevel : std::uint8_t {
    none = 0,
    beforeData = 1,
    afterSequence = 2,
};

class CancelToken {
public:
    void request(CancelLevel level) noexcept
    {
        auto current = level_.load();
        while (static_cast<std::uint8_t>(level) >
                   static_cast<std::uint8_t>(current) &&
               !level_.compare_exchange_weak(current, level)) {
        }
    }

    // Clears any request. Callers reset explicitly before a new operation; a
    // stale request otherwise stops the next operation before data (fail-safe).
    void reset() noexcept { level_.store(CancelLevel::none); }

    CancelLevel level() const noexcept { return level_.load(); }

    bool honoured(bool dataSent) const noexcept
    {
        const auto current = level_.load();
        return dataSent ? current == CancelLevel::afterSequence
                        : current != CancelLevel::none;
    }

private:
    std::atomic<CancelLevel> level_{CancelLevel::none};
};

} // namespace bifrost

#endif
