// Fallback fuzz driver for toolchains without libFuzzer (e.g. Apple clang,
// which ships ASan/UBSan but not the libFuzzer runtime). It replays every
// corpus file, then runs a simple random-mutation loop. It is much weaker
// than libFuzzer (no coverage guidance); CI uses real libFuzzer.
//
// Usage: fuzz_X [-runs=N] [-max_total_time=S] [-max_len=L] [-seed=N] PATH...
// PATH may be a file or a directory of seed files. Unknown -flags are ignored
// so the same command line works with libFuzzer.
#include <sanitizer/common_interface_defs.h>

#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <random>
#include <string>
#include <vector>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size);

namespace {

using Bytes = std::vector<std::uint8_t>;

const Bytes* gCurrent = nullptr;

void saveCrash()
{
    if (gCurrent == nullptr) {
        return;
    }
    std::uint64_t hash = 1469598103934665603ull;
    for (auto b : *gCurrent) {
        hash = (hash ^ b) * 1099511628211ull;
    }
    char name[64];
    std::snprintf(name, sizeof name, "crash-%016llx", static_cast<unsigned long long>(hash));
    std::ofstream(name, std::ios::binary)
        .write(reinterpret_cast<const char*>(gCurrent->data()),
               static_cast<std::streamsize>(gCurrent->size()));
    std::fprintf(stderr, "==standalone== crashing input saved to %s (%zu bytes)\n", name,
                 gCurrent->size());
}

// __builtin_trap (SIGILL) and plain aborts bypass the sanitizer callback.
// Not async-signal-safe, which is acceptable for a dying fuzz process.
void onFatalSignal(int signal)
{
    saveCrash();
    std::signal(signal, SIG_DFL);
    std::raise(signal);
}

void run(const Bytes& input)
{
    gCurrent = &input;
    // Copy into an exact-size heap buffer so ASan catches over-reads.
    std::unique_ptr<std::uint8_t[]> copy(new std::uint8_t[input.size() + 1]);
    std::memcpy(copy.get(), input.data(), input.size());
    LLVMFuzzerTestOneInput(input.empty() ? nullptr : copy.get(), input.size());
    gCurrent = nullptr;
}

Bytes readFile(const std::filesystem::path& path)
{
    std::ifstream file(path, std::ios::binary);
    return Bytes(std::istreambuf_iterator<char>(file), {});
}

const std::uint32_t kInteresting32[] = {0,          1,          0x7F,       0x80,
                                        0xFF,       0x100,      0x1F4,      0x200,
                                        0x7FFF,     0x8000,     0xFFFF,     0x10000,
                                        0x7FFFFFFF, 0x80000000, 0xFFFFFFFF, 0x12349876};

void mutate(Bytes& data, const std::vector<Bytes>& corpus, std::mt19937_64& rng,
            std::size_t maxLength)
{
    auto pick = [&](std::size_t n) { return n == 0 ? 0 : static_cast<std::size_t>(rng() % n); };
    const int count = 1 + static_cast<int>(rng() % 4);
    for (int i = 0; i < count; ++i) {
        switch (rng() % 8) {
        case 0: // flip a bit
            if (!data.empty()) {
                data[pick(data.size())] ^= static_cast<std::uint8_t>(1u << (rng() % 8));
            }
            break;
        case 1: // random byte
            if (!data.empty()) {
                data[pick(data.size())] = static_cast<std::uint8_t>(rng());
            }
            break;
        case 2: { // insert bytes
            const auto at = pick(data.size() + 1);
            const auto n = 1 + pick(16);
            Bytes insert(n);
            for (auto& b : insert) {
                b = static_cast<std::uint8_t>(rng());
            }
            data.insert(data.begin() + static_cast<std::ptrdiff_t>(at), insert.begin(),
                        insert.end());
            break;
        }
        case 3: // erase a range
            if (!data.empty()) {
                const auto at = pick(data.size());
                const auto n = 1 + pick(std::min<std::size_t>(64, data.size() - at));
                data.erase(data.begin() + static_cast<std::ptrdiff_t>(at),
                           data.begin() + static_cast<std::ptrdiff_t>(at + n));
            }
            break;
        case 4: // interesting 32-bit little-endian value
            if (data.size() >= 4) {
                const auto at = pick(data.size() - 3);
                const auto v = kInteresting32[pick(std::size(kInteresting32))];
                for (int k = 0; k < 4; ++k) {
                    data[at + static_cast<std::size_t>(k)] = static_cast<std::uint8_t>(v >> (8 * k));
                }
            }
            break;
        case 5: // ASCII digit / octal-ish byte (TAR headers)
            if (!data.empty()) {
                data[pick(data.size())] = static_cast<std::uint8_t>("0123456789 \0x7"[pick(14)]);
            }
            break;
        case 6: { // splice another corpus entry
            if (!corpus.empty()) {
                const auto& other = corpus[pick(corpus.size())];
                if (!other.empty()) {
                    const auto from = pick(other.size());
                    const auto n = 1 + pick(other.size() - from);
                    const auto at = pick(data.size() + 1);
                    data.insert(data.begin() + static_cast<std::ptrdiff_t>(at),
                                other.begin() + static_cast<std::ptrdiff_t>(from),
                                other.begin() + static_cast<std::ptrdiff_t>(from + n));
                }
            }
            break;
        }
        default: // truncate
            if (!data.empty()) {
                data.resize(pick(data.size()));
            }
            break;
        }
    }
    if (data.size() > maxLength) {
        data.resize(maxLength);
    }
}

} // namespace

int main(int argc, char** argv)
{
    __sanitizer_set_death_callback(saveCrash);
    for (int signal : {SIGILL, SIGABRT, SIGSEGV, SIGBUS, SIGFPE, SIGTRAP}) {
        std::signal(signal, onFatalSignal);
    }
    std::uint64_t runs = 100000;
    double maxSeconds = 0;
    std::size_t maxLength = 65536;
    std::uint64_t seed = 1;
    std::vector<Bytes> corpus;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg.rfind("-runs=", 0) == 0) {
            runs = std::strtoull(arg.c_str() + 6, nullptr, 10);
        } else if (arg.rfind("-max_total_time=", 0) == 0) {
            maxSeconds = std::strtod(arg.c_str() + 16, nullptr);
        } else if (arg.rfind("-max_len=", 0) == 0) {
            maxLength = std::strtoull(arg.c_str() + 9, nullptr, 10);
        } else if (arg.rfind("-seed=", 0) == 0) {
            seed = std::strtoull(arg.c_str() + 6, nullptr, 10);
        } else if (!arg.empty() && arg[0] == '-') {
            continue;
        } else if (std::filesystem::is_directory(arg)) {
            for (const auto& item : std::filesystem::directory_iterator(arg)) {
                if (item.is_regular_file()) {
                    corpus.push_back(readFile(item.path()));
                }
            }
        } else if (std::filesystem::is_regular_file(arg)) {
            corpus.push_back(readFile(arg));
        }
    }
    for (const auto& input : corpus) {
        run(input);
    }
    std::cerr << "standalone: replayed " << corpus.size() << " corpus inputs\n";

    std::mt19937_64 rng(seed);
    const auto start = std::chrono::steady_clock::now();
    std::uint64_t done = 0;
    for (; done < runs; ++done) {
        if (maxSeconds > 0 && done % 64 == 0 &&
            std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count() >
                maxSeconds) {
            break;
        }
        Bytes input = corpus.empty() ? Bytes{} : corpus[static_cast<std::size_t>(rng() % corpus.size())];
        mutate(input, corpus, rng, maxLength);
        run(input);
    }
    std::cerr << "standalone: " << done << " mutated runs, no crash (seed " << seed << ")\n";
    return 0;
}
