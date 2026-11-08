#ifndef BIFROST_COMMON_ERROR_HPP
#define BIFROST_COMMON_ERROR_HPP

#include <cstdint>
#include <string>
#include <utility>
#include <variant>

namespace bifrost {

// Stable error classification used by every layer. Recovery decisions are
// made from the code (and DataState), never from message text.
enum class ErrorCode : std::uint16_t {
    invalidArgument = 1,
    ioError,
    packageInvalid,
    checksumMismatch,
    archiveUnsupported,
    lz4Invalid,
    insufficientSpace,
    fileChanged,
    pitInvalid,
    planRefused,
    modelMismatch,
    usbDisconnected,
    usbTimeout,
    usbStall,
    usbAccess,
    usbOther,
    protocolViolation,
    bootloaderRejected,
    cancelled,
    illegalState,
    serviceUnavailable, // the background agent or USB helper is not reachable
};

// What the device may have received when an operation failed.
enum class DataState : std::uint8_t {
    none,     // no partition data was sent
    partial,  // some partition data may have been written
    complete, // all data was acknowledged; only session close/reboot failed
};

struct Error {
    ErrorCode code{ErrorCode::ioError};
    std::string message;
    DataState dataState{DataState::none};
    int nativeCode{};
};

const char* toString(ErrorCode code) noexcept;
const char* toString(DataState state) noexcept;

inline Error makeError(ErrorCode code, std::string message, int nativeCode = 0)
{
    return Error{code, std::move(message), DataState::none, nativeCode};
}

// Minimal expected<T, Error> for C++17.
template <typename T>
class Expected {
public:
    Expected(T value) : storage_(std::in_place_index<0>, std::move(value)) {}
    Expected(Error error) : storage_(std::in_place_index<1>, std::move(error)) {}

    explicit operator bool() const noexcept { return storage_.index() == 0; }
    bool ok() const noexcept { return storage_.index() == 0; }

    T& value() & { return std::get<0>(storage_); }
    const T& value() const& { return std::get<0>(storage_); }
    T&& value() && { return std::get<0>(std::move(storage_)); }
    T* operator->() { return &std::get<0>(storage_); }
    const T* operator->() const { return &std::get<0>(storage_); }
    T& operator*() & { return std::get<0>(storage_); }
    const T& operator*() const& { return std::get<0>(storage_); }

    const Error& error() const& { return std::get<1>(storage_); }
    Error&& error() && { return std::get<1>(std::move(storage_)); }

private:
    std::variant<T, Error> storage_;
};

struct Ok {};
using Status = Expected<Ok>;

inline Status success() { return Status(Ok{}); }

} // namespace bifrost

#endif
