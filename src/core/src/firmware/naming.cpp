#include "bifrost/firmware/naming.hpp"

#include <algorithm>
#include <cctype>
#include <vector>

namespace bifrost::firmware {
namespace {

std::string upper(std::string text)
{
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) {
        return static_cast<char>(std::toupper(c));
    });
    return text;
}

std::vector<std::string> splitTokens(const std::string& text)
{
    std::vector<std::string> tokens;
    std::string current;
    for (const char c : text) {
        if (c == '_' || c == '-' || c == '.') {
            if (!current.empty()) {
                tokens.push_back(current);
            }
            current.clear();
        } else {
            current.push_back(c);
        }
    }
    if (!current.empty()) {
        tokens.push_back(current);
    }
    return tokens;
}

bool alnum(const std::string& text)
{
    return !text.empty() &&
           std::all_of(text.begin(), text.end(),
                       [](unsigned char c) { return std::isalnum(c) != 0; });
}

std::optional<int> base36(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'A' && c <= 'Z') {
        return c - 'A' + 10;
    }
    return std::nullopt;
}

// Modern builds have an 8-character suffix after the model code:
//   AP/BL/CP: <2 region chars><U|S|E><bootloader rev><4 chars>  (XXU8DUH1)
//   CSC:      <3-char CSC code><bootloader rev><4 chars>        (OXM8DUH1)
void inferModel(FirmwareName& name)
{
    const auto& build = name.build;
    if (build.size() < 12 || !alnum(build)) {
        return;
    }
    const auto suffix = build.substr(build.size() - 8);
    const bool cscBuild = !name.cscCode.empty() && suffix.compare(0, 3, name.cscCode) == 0;
    if (!cscBuild && suffix[2] != 'U' && suffix[2] != 'S' && suffix[2] != 'E') {
        return;
    }
    const auto model = build.substr(0, build.size() - 8);
    if (model.size() < 4 || std::isalpha(static_cast<unsigned char>(model[0])) == 0) {
        return;
    }
    name.modelCode = model;
    name.bootloaderRevision = base36(suffix[3]);
}

} // namespace

const char* toString(PackageKind kind) noexcept
{
    switch (kind) {
    case PackageKind::bl: return "BL";
    case PackageKind::ap: return "AP";
    case PackageKind::cp: return "CP";
    case PackageKind::csc: return "CSC";
    case PackageKind::homeCsc: return "HOME_CSC";
    case PackageKind::userdata: return "USERDATA";
    case PackageKind::unknown: return "unknown";
    }
    return "unknown";
}

FirmwareName parseFirmwareName(const std::string& path)
{
    const auto slash = path.find_last_of('/');
    auto filename = upper(slash == std::string::npos ? path : path.substr(slash + 1));
    FirmwareName name;
    const auto tokens = splitTokens(filename);
    if (tokens.empty()) {
        return name;
    }

    std::size_t buildIndex = 0;
    if (tokens.size() >= 3 && tokens[0] == "HOME" && tokens[1] == "CSC") {
        name.kind = PackageKind::homeCsc;
        name.cscCode = tokens.size() > 3 ? tokens[2] : "";
        buildIndex = 3;
    } else if (tokens[0] == "CSC") {
        name.kind = PackageKind::csc;
        name.cscCode = tokens.size() > 2 ? tokens[1] : "";
        buildIndex = 2;
    } else if (tokens[0] == "BL" || tokens[0] == "AP" || tokens[0] == "CP") {
        name.kind = tokens[0] == "BL"   ? PackageKind::bl
                    : tokens[0] == "AP" ? PackageKind::ap
                                        : PackageKind::cp;
        buildIndex = 1;
    } else if (tokens[0] == "USERDATA") {
        name.kind = PackageKind::userdata;
        buildIndex = 1;
    }
    if (name.cscCode.size() != 3 || !alnum(name.cscCode)) {
        name.cscCode.clear();
    }
    if (buildIndex < tokens.size()) {
        name.build = tokens[buildIndex];
        inferModel(name);
    }
    return name;
}

std::string normalizeModel(const std::string& model)
{
    std::string result;
    for (const char c : upper(model)) {
        if (std::isalnum(static_cast<unsigned char>(c)) != 0) {
            result.push_back(c);
        }
    }
    if (result.compare(0, 2, "SM") == 0 && result.size() > 2) {
        result.erase(0, 2);
    }
    return result;
}

} // namespace bifrost::firmware
