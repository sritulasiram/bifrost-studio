#include "bifrost/transport/iokit_registry.hpp"

#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>

#include <optional>
#include <string>

namespace bifrost::transport {
namespace {

constexpr std::int32_t kSamsungVendorId = 0x04E8;

std::optional<std::int32_t> readIntegerProperty(io_service_t service,
                                                CFStringRef propertyName)
{
    CFTypeRef property = IORegistryEntryCreateCFProperty(
        service, propertyName, kCFAllocatorDefault, 0);
    if (property == nullptr) {
        return std::nullopt;
    }

    std::optional<std::int32_t> value;
    if (CFGetTypeID(property) == CFNumberGetTypeID()) {
        std::int32_t number = 0;
        if (CFNumberGetValue(static_cast<CFNumberRef>(property),
                             kCFNumberSInt32Type, &number)) {
            value = number;
        }
    }
    CFRelease(property);
    return value;
}

Expected<std::size_t> countOnServiceClass(const char* className)
{
    CFMutableDictionaryRef matching = IOServiceMatching(className);
    if (matching == nullptr) {
        return makeError(ErrorCode::ioError,
            std::string("IOKit could not create matching dictionary for ") +
            className);
    }

    io_iterator_t iterator = IO_OBJECT_NULL;
    const kern_return_t result =
        IOServiceGetMatchingServices(kIOMainPortDefault, matching, &iterator);
    if (result != KERN_SUCCESS) {
        return makeError(ErrorCode::ioError,
            std::string("IOKit enumeration failed for ") + className, result);
    }

    std::size_t count = 0;
    io_service_t service = IOIteratorNext(iterator);
    while (service != IO_OBJECT_NULL) {
        const auto vendor =
            readIntegerProperty(service, CFSTR("idVendor"));
        if (vendor && *vendor == kSamsungVendorId) {
            ++count;
        }
        IOObjectRelease(service);
        service = IOIteratorNext(iterator);
    }
    IOObjectRelease(iterator);

    return count;
}

} // namespace

Expected<std::size_t> countSamsungDevicesInIokitRegistry()
{
    auto hostResult = countOnServiceClass("IOUSBHostDevice");
    if (hostResult && *hostResult > 0) {
        return hostResult;
    }
    auto legacyResult = countOnServiceClass("IOUSBDevice");
    if (legacyResult || !hostResult) {
        return legacyResult;
    }
    return hostResult;
}

} // namespace bifrost::transport
