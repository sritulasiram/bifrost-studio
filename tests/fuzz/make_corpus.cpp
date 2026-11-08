// Writes the seed corpora under tests/fuzz/corpus/. Run after changing the
// builders; commit the output. Usage: make_fuzz_corpus OUTPUT_DIR
#include "support/test_support.hpp"

#include <lz4frame.h>

#include <filesystem>

using test::Bytes;
using test::TarBuilder;

namespace {

std::filesystem::path gRoot;

void write(const std::string& target, const std::string& name, const Bytes& bytes)
{
    const auto dir = gRoot / target;
    std::filesystem::create_directories(dir);
    std::ofstream(dir / name, std::ios::binary)
        .write(reinterpret_cast<const char*>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
}

Bytes prefixed(std::uint8_t mode, const Bytes& bytes)
{
    Bytes out{mode};
    out.insert(out.end(), bytes.begin(), bytes.end());
    return out;
}

Bytes lz4(const Bytes& source, bool contentSize, bool checksum, LZ4F_blockSizeID_t block)
{
    LZ4F_preferences_t p{};
    p.frameInfo.blockSizeID = block;
    p.frameInfo.contentSize = contentSize ? source.size() : 0;
    p.frameInfo.contentChecksumFlag =
        checksum ? LZ4F_contentChecksumEnabled : LZ4F_noContentChecksum;
    Bytes out(LZ4F_compressFrameBound(source.size(), &p));
    out.resize(LZ4F_compressFrame(out.data(), out.size(), source.data(), source.size(), &p));
    return out;
}

Bytes text(const std::string& s) { return Bytes(s.begin(), s.end()); }

} // namespace

int main(int argc, char** argv)
{
    if (argc != 2) {
        std::cerr << "usage: make_fuzz_corpus OUTPUT_DIR\n";
        return 2;
    }
    gRoot = argv[1];

    // PIT
    const std::vector<test::PitSpec> layout{
        {1, "BOOT", "boot.img"}, {2, "SYSTEM", "system.img"}, {3, "MODEM", "modem.bin", 1, 2}};
    write("pit", "empty", test::makePit({}));
    write("pit", "three", test::makePit(layout));
    write("pit", "one", test::makePit({{80, "SUPER", "super.img", 0, 8, 0x7FFFFF}}));
    auto truncated = test::makePit(layout);
    truncated.resize(100);
    write("pit", "truncated", truncated);

    // Packages (first byte selects .tar / raw .tar.md5 / .tar.md5 + valid trailer)
    const Bytes image(700, 0x5A);
    const auto simple = TarBuilder()
                            .file("boot.img", image)
                            .file("system.img.lz4", lz4(image, true, true, LZ4F_max64KB))
                            .file("meta-data/fota.zip", Bytes(16, 3))
                            .finish();
    write("package", "tar-simple", prefixed(0, simple));
    write("package", "md5-gate", prefixed(2, simple));
    write("package", "md5-trailer", prefixed(1, test::withMd5Trailer(simple, "AP.tar.md5")));
    write("package", "gnu-longname",
          prefixed(0, TarBuilder().file(std::string(140, 'a') + ".img", image).finish()));
    write("package", "pax-path",
          prefixed(0, TarBuilder().pax("path", "dir/pax-name.img").file("x", image).finish()));
    write("package", "base256",
          prefixed(0, TarBuilder()
                          .file("big.img", image, TarBuilder::SizeEncoding::base256)
                          .finish()));
    write("package", "traversal", prefixed(0, TarBuilder().file("../evil.img", image).finish()));
    write("package", "directory",
          prefixed(0, TarBuilder().file("dir/", {}, TarBuilder::SizeEncoding::octal, '5')
                          .file("dir/a.img", image)
                          .finish()));

    // LZ4 frames
    Bytes pattern(150000);
    for (std::size_t i = 0; i < pattern.size(); ++i) {
        pattern[i] = static_cast<std::uint8_t>((i / 3) ^ (i >> 9));
    }
    write("lz4", "small-checked", lz4(Bytes(1000, 7), true, true, LZ4F_max64KB));
    write("lz4", "no-size", lz4(Bytes(1000, 7), false, false, LZ4F_max64KB));
    write("lz4", "empty-frame", lz4({}, true, true, LZ4F_max64KB));
    write("lz4", "multi-block", lz4(pattern, true, true, LZ4F_max64KB));
    write("lz4", "big-block", lz4(pattern, false, true, LZ4F_max4MB));

    // File names
    const char* names[] = {
        "AP_N950FXXU8DUH1_CL21848355_QB43397000_REV00_user_low_ship_meta_OS9.tar.md5",
        "BL_N950FXXU8DUH1_CL21848355_QB43397000_REV00_user_low_ship.tar.md5",
        "CP_N950FXXU8DUH1_CP19946543_CL21848355_QB43397000_REV00_user_low_ship.tar.md5",
        "CSC_OXM_N950FOXM8DUH1_CL21848355_QB43397000_REV00_user_low_ship.tar.md5",
        "HOME_CSC_OXM_N986BOXM4HWG1_CL25051001_QB60009466_REV00_user_low_ship.tar.md5",
        "USERDATA_N915FXXU1BOB4.tar",
        "sm-n950f",
        "SM-N986B/DS",
        "AP_.tar.md5",
    };
    int index = 0;
    for (const char* name : names) {
        write("naming", "name" + std::to_string(index++), text(name));
    }
    std::cout << "seed corpora written to " << gRoot << '\n';
    return 0;
}
