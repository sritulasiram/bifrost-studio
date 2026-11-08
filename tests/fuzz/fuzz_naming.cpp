// Firmware file-name heuristics and model normalisation.
#include "bifrost/firmware/naming.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <cctype>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size)
{
    const std::string text(reinterpret_cast<const char*>(data), size);
    const auto name = bifrost::firmware::parseFirmwareName(text);
    if (name.bootloaderRevision && *name.bootloaderRevision < 0) {
        __builtin_trap();
    }
    // Not idempotent by design: one leading "SM" is stripped per call, so
    // "SMSM-N950F" -> "SMN950F", which then fails the model match (fail-safe).
    const auto model = bifrost::firmware::normalizeModel(text);
    for (const char c : model) {
        if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))) {
            __builtin_trap(); // only uppercase ASCII letters and digits
        }
    }
    return 0;
}
