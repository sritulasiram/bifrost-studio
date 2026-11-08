#include "bifrost/common/error.hpp"

namespace bifrost {

const char* toString(ErrorCode code) noexcept
{
    switch (code) {
    case ErrorCode::invalidArgument: return "invalid-argument";
    case ErrorCode::ioError: return "io-error";
    case ErrorCode::packageInvalid: return "package-invalid";
    case ErrorCode::checksumMismatch: return "checksum-mismatch";
    case ErrorCode::archiveUnsupported: return "archive-unsupported";
    case ErrorCode::lz4Invalid: return "lz4-invalid";
    case ErrorCode::insufficientSpace: return "insufficient-space";
    case ErrorCode::fileChanged: return "file-changed";
    case ErrorCode::pitInvalid: return "pit-invalid";
    case ErrorCode::planRefused: return "plan-refused";
    case ErrorCode::modelMismatch: return "model-mismatch";
    case ErrorCode::usbDisconnected: return "usb-disconnected";
    case ErrorCode::usbTimeout: return "usb-timeout";
    case ErrorCode::usbStall: return "usb-stall";
    case ErrorCode::usbAccess: return "usb-access";
    case ErrorCode::usbOther: return "usb-other";
    case ErrorCode::protocolViolation: return "protocol-violation";
    case ErrorCode::bootloaderRejected: return "bootloader-rejected";
    case ErrorCode::cancelled: return "cancelled";
    case ErrorCode::illegalState: return "illegal-state";
    case ErrorCode::serviceUnavailable: return "service-unavailable";
    }
    return "unknown";
}

const char* toString(DataState state) noexcept
{
    switch (state) {
    case DataState::none: return "no data sent";
    case DataState::partial: return "partially written";
    case DataState::complete: return "all data acknowledged";
    }
    return "unknown";
}

} // namespace bifrost
