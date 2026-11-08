// Firmware package reader: .tar.md5 verification, TAR parsing (including GNU
// and PAX extensions), unsafe input rejection, change detection, naming, LZ4.
#include "bifrost/firmware/lz4.hpp"
#include "bifrost/firmware/naming.hpp"
#include "bifrost/firmware/package.hpp"
#include "support/test_support.hpp"

#include <lz4frame.h>
#include <thread>

using namespace bifrost;
using test::Bytes;
using test::TarBuilder;

namespace {

Bytes lz4Compress(const Bytes& source, bool contentSize)
{
    LZ4F_preferences_t preferences{};
    preferences.frameInfo.contentSize = contentSize ? source.size() : 0;
    Bytes out(LZ4F_compressFrameBound(source.size(), &preferences));
    const auto size = LZ4F_compressFrame(out.data(), out.size(), source.data(),
                                         source.size(), &preferences);
    out.resize(LZ4F_isError(size) ? 0 : size);
    return out;
}

} // namespace

int main()
{
    test::TempDir dir;
    const Bytes boot{'b', 'o', 'o', 't', '!'};

    // Plain .tar.
    {
        const auto path = dir.write("plain.tar", TarBuilder().file("boot.img", boot).finish());
        auto package = firmware::inspectPackage(path);
        CHECK(package && package->entries.size() == 1 && !package->checksumPresent);
        Bytes read(boot.size());
        CHECK(package && firmware::readEntryBytes(*package, package->entries[0], 0,
                                                  read.data(), read.size()));
        CHECK(read == boot);
    }

    // .tar.md5: valid, progress reported, and tampered.
    {
        const auto tar = TarBuilder().file("boot.img", boot).finish();
        const auto path = dir.write("ok.tar.md5", test::withMd5Trailer(tar, "ok.tar"));
        std::uint64_t reported = 0;
        auto package = firmware::inspectPackage(
            path, [&](std::uint64_t done, std::uint64_t) { reported = done; });
        CHECK(package && package->checksumValid && package->checksumHex == test::md5Hex(tar));
        CHECK(reported == tar.size());

        auto tampered = test::withMd5Trailer(tar, "ok.tar");
        tampered[600] ^= 1;
        auto bad = firmware::inspectPackage(dir.write("bad.tar.md5", tampered));
        CHECK(!bad && bad.error().code == ErrorCode::checksumMismatch);

        auto missing = firmware::inspectPackage(dir.write("none.tar.md5", tar));
        CHECK(!missing && missing.error().code == ErrorCode::packageInvalid);
    }

    // Cancellation during hashing.
    {
        const auto tar = TarBuilder().file("boot.img", boot).finish();
        const auto path = dir.write("cancel.tar.md5", test::withMd5Trailer(tar, "c.tar"));
        CancelToken cancel;
        cancel.request(CancelLevel::beforeData);
        auto package = firmware::inspectPackage(path, {}, &cancel);
        CHECK(!package && package.error().code == ErrorCode::cancelled);
    }

    // GNU long name, PAX path/size, and base-256 size.
    {
        const std::string longName(130, 'a');
        const auto tar = TarBuilder()
                             .file(longName + ".img", boot)
                             .pax("path", "pax-name.img")
                             .file("ignored", boot)
                             .file("big.img", boot, TarBuilder::SizeEncoding::base256)
                             .file("dir/", {}, TarBuilder::SizeEncoding::octal, '5')
                             .finish();
        auto package = firmware::inspectPackage(dir.write("ext.tar", tar));
        CHECK(package && package->entries.size() == 3);
        if (package && package->entries.size() == 3) {
            CHECK(package->entries[0].name == longName + ".img");
            CHECK(package->entries[1].name == "pax-name.img");
            CHECK(package->entries[2].name == "big.img" && package->entries[2].size == 5);
        }
    }

    // Unsafe and unsupported input.
    {
        auto traversal = firmware::inspectPackage(
            dir.write("evil.tar", TarBuilder().file("../evil", boot).finish()));
        CHECK(!traversal);
        auto absolute = firmware::inspectPackage(
            dir.write("abs.tar", TarBuilder().file("/etc/x", boot).finish()));
        CHECK(!absolute);
        auto link = firmware::inspectPackage(dir.write(
            "link.tar",
            TarBuilder().file("boot.img", {}, TarBuilder::SizeEncoding::octal, '2').finish()));
        CHECK(!link && link.error().code == ErrorCode::archiveUnsupported);
        auto truncated = TarBuilder().file("boot.img", Bytes(2000, 1)).finish();
        truncated.resize(1024);
        CHECK(!firmware::inspectPackage(dir.write("trunc.tar", truncated)));
        CHECK(!firmware::inspectPackage(dir.write("wrong.zip", Bytes(2048, 0))));
    }

    // A file replaced after verification is detected.
    {
        const auto path = dir.write("change.tar", TarBuilder().file("boot.img", boot).finish());
        auto package = firmware::inspectPackage(path);
        CHECK(package && package->source->verifyUnchanged());
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        dir.write("change.tar", TarBuilder().file("boot.img", Bytes{'x'}).finish());
        CHECK(package && !package->source->verifyUnchanged());
    }

    // LZ4: validation, streaming, and corruption.
    {
        Bytes image(3 * 1024 * 1024 + 17);
        for (std::size_t i = 0; i < image.size(); ++i) {
            image[i] = static_cast<std::uint8_t>((i * 31) ^ (i >> 9));
        }
        for (const bool declared : {true, false}) {
            const auto compressed = lz4Compress(image, declared);
            const auto path = dir.write(declared ? "d.tar" : "u.tar",
                                        TarBuilder().file("system.img.lz4", compressed).finish());
            auto package = firmware::inspectPackage(path);
            CHECK(package && package->entries[0].lz4Compressed);
            auto size = firmware::validateLz4(*package, package->entries[0]);
            CHECK(size && *size == image.size());
            auto shared = std::make_shared<const firmware::Package>(*package);
            auto reader = firmware::Lz4StreamReader::open(shared, package->entries[0],
                                                          image.size());
            CHECK(reader);
            Bytes decoded(image.size());
            std::size_t offset = 0;
            while (reader && offset < decoded.size()) {
                const auto chunk = std::min<std::size_t>(1048576, decoded.size() - offset);
                CHECK((*reader)->read(offset, decoded.data() + offset, chunk));
                offset += chunk;
            }
            CHECK(decoded == image);
            if (reader) {
                CHECK(!(*reader)->read(0, decoded.data(), 1)); // non-sequential
            }
        }
        auto corrupt = lz4Compress(image, true);
        corrupt[corrupt.size() / 2] ^= 0xFF;
        auto package = firmware::inspectPackage(
            dir.write("bad-lz4.tar", TarBuilder().file("x.img.lz4", corrupt).finish()));
        CHECK(package && !firmware::validateLz4(*package, package->entries[0]));
    }

    // Samsung file-name parsing.
    {
        auto ap = firmware::parseFirmwareName(
            "/x/AP_N950FXXU8DUH1_CL1_QB2_REV00_user_low_ship_meta_OS9.tar.md5");
        CHECK(ap.kind == firmware::PackageKind::ap && ap.modelCode == "N950F" &&
              ap.bootloaderRevision && *ap.bootloaderRevision == 8);
        auto home = firmware::parseFirmwareName("HOME_CSC_OXM_N950FOXM8DUH1_CL1.tar.md5");
        CHECK(home.kind == firmware::PackageKind::homeCsc && home.cscCode == "OXM" &&
              home.modelCode == "N950F");
        auto csc = firmware::parseFirmwareName("CSC_OXM_N986BOXM3DVA1_CL1.tar.md5");
        CHECK(csc.kind == firmware::PackageKind::csc && csc.modelCode == "N986B");
        auto custom = firmware::parseFirmwareName("my-recovery.tar");
        CHECK(custom.kind == firmware::PackageKind::unknown && custom.modelCode.empty());
        CHECK(firmware::normalizeModel("sm-n950f") == "N950F");
        CHECK(firmware::normalizeModel(" SM-N950F ") == "N950F");
        CHECK(firmware::normalizeModel("N950F") == "N950F");
    }

    return test::result();
}
