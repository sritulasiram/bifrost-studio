// PIT parser: untrusted bytes from the device or a user-supplied file.
#include "bifrost/pit/pit.hpp"

#include <cstddef>
#include <cstdint>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size)
{
    auto table = bifrost::pit::parse(data, size);
    if (table) {
        for (const auto& entry : table->entries) {
            (void)table->findByIdentifier(entry.identifier);
            (void)table->findByName(entry.partitionName);
        }
        const auto comparison = bifrost::pit::compare(*table, *table);
        if (!comparison.sameLayout() || !comparison.sizeDifferences.empty()) {
            __builtin_trap(); // a table must equal itself
        }
    }
    return 0;
}
