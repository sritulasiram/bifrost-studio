#ifndef BIFROST_FIRMWARE_NAMING_HPP
#define BIFROST_FIRMWARE_NAMING_HPP

#include <optional>
#include <string>

namespace bifrost::firmware {

enum class PackageKind {
    unknown,
    bl,
    ap,
    cp,
    csc,      // full CSC: may erase user data
    homeCsc,  // HOME_CSC: intended to keep user data
    userdata,
};

const char* toString(PackageKind kind) noexcept;

// Information inferred from Samsung's firmware file-naming convention, e.g.
//   AP_N950FXXU8DUH1_..._meta_OS9.tar.md5
//   HOME_CSC_OXM_N950FOXM8DUH1_...tar.md5
// This is a heuristic (UNVERIFIED for every model family). It is used for
// cross-slot consistency checks and to pre-fill the model; it is never the
// only evidence that a firmware matches a device.
struct FirmwareName {
    PackageKind kind{PackageKind::unknown};
    std::string build;        // e.g. N950FXXU8DUH1
    std::string modelCode;    // e.g. N950F (empty if not inferable)
    std::string cscCode;      // e.g. OXM (CSC packages only)
    std::optional<int> bootloaderRevision; // the "8" in XXU8..., base 36
};

FirmwareName parseFirmwareName(const std::string& path);

// Normalises a user-typed model ("sm-n950f", "SM-N950F", "N950F") to the
// bare model code ("N950F").
std::string normalizeModel(const std::string& model);

} // namespace bifrost::firmware

#endif
