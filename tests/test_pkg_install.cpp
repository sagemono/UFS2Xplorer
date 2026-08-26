#include "ufs2_test_image.h"

#include <ps3hdd_crypto/aes_ctr_128.h>
#include <ps3hdd_crypto/be_io.h>
#include <ps3hdd_pkg/pkg_installer.h>
#include <ps3hdd_pkg/ps3_pkg_reader.h>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstring>
#include <string>
#include <tuple>
#include <vector>

using namespace ps3hdd;
using ps3hdd::write_be_u32;
using ps3hdd::write_be_u64;

namespace {

constexpr std::array<std::byte, 16> kPs3AesKey = {
    std::byte{0x2E}, std::byte{0x7B}, std::byte{0x71}, std::byte{0xD7},
    std::byte{0xC9}, std::byte{0xC9}, std::byte{0xA1}, std::byte{0x4E},
    std::byte{0xA3}, std::byte{0x22}, std::byte{0x1F}, std::byte{0x18},
    std::byte{0x88}, std::byte{0x28}, std::byte{0xB8}, std::byte{0xF8}
};

struct test_pkg {
    std::vector<std::byte> bytes;
    std::vector<std::byte> param_sfo; // PARAM.SFO plaintext
    std::vector<std::byte> eboot; // USRDIR/EBOOT.BIN plaintext
};

test_pkg build_test_pkg() {
    test_pkg pkg;
    pkg.param_sfo = tuf::pattern(5000, 1);
    pkg.eboot = tuf::pattern(40000, 2);

    const std::string name0 = "PARAM.SFO";
    const std::string name1 = "USRDIR/EBOOT.BIN";

    const std::uint64_t name0_off = 64, name1_off = 96;
    const std::uint64_t data0_off = 128;
    const std::uint64_t data1_off = (data0_off + pkg.param_sfo.size() + 15) & ~std::uint64_t{15};
    const std::uint64_t region_size = data1_off + pkg.eboot.size();

    std::vector<std::byte> region(region_size, std::byte{0});
    auto put_entry = [&](std::size_t o, std::uint32_t noff, std::uint32_t nsize, std::uint64_t doff, std::uint64_t dsize) {
        write_be_u32(region.data() + o, noff);
        write_be_u32(region.data() + o + 4, nsize);
        write_be_u64(region.data() + o + 8, doff);
        write_be_u64(region.data() + o + 16, dsize);
        // content_type @24, file_type @27 both 0 (regular file)
    };
    put_entry(0, static_cast<std::uint32_t>(name0_off), static_cast<std::uint32_t>(name0.size()), data0_off, pkg.param_sfo.size());
    put_entry(32, static_cast<std::uint32_t>(name1_off), static_cast<std::uint32_t>(name1.size()), data1_off, pkg.eboot.size());
    std::memcpy(region.data() + name0_off, name0.data(), name0.size());
    std::memcpy(region.data() + name1_off, name1.data(), name1.size());
    std::memcpy(region.data() + data0_off, pkg.param_sfo.data(), pkg.param_sfo.size());
    std::memcpy(region.data() + data1_off, pkg.eboot.data(), pkg.eboot.size());

    // encrypt the whole region as one CTR stream (encrypt == decrypt)
    std::array<std::byte, 16> riv{};
    for (int i = 0; i < 16; ++i) riv[i] = static_cast<std::byte>(0xA0 + i);
    std::vector<std::byte> enc(region_size);
    crypto::aes_ctr_128 cipher{std::span<const std::byte>(kPs3AesKey)};
    cipher.process(region, enc, riv, 0);

    // assemble the .pkg: 0x100 byte header area + the encrypted region
    const std::uint64_t data_offset = 0x100;
    pkg.bytes.assign(data_offset + region_size, std::byte{0});
    std::byte* h = pkg.bytes.data();
    h[0] = std::byte{0x7F}; h[1] = std::byte{0x50}; h[2] = std::byte{0x4B}; h[3] = std::byte{0x47};
    h[4] = std::byte{0x80}; // finalized -> AES-CTR
    h[7] = std::byte{0x01}; // PS3
    write_be_u32(h + 0x14, 2); // file count
    write_be_u64(h + 0x20, data_offset);
    write_be_u64(h + 0x28, region_size);
    const std::string cid = "UP0001-TEST12345_00-0000000000000000";
    std::memcpy(h + 0x30, cid.data(), cid.size());
    std::memcpy(h + 0x70, riv.data(), 16);
    std::memcpy(h + data_offset, enc.data(), region_size);
    return pkg;
}

} // namespace

TEST_CASE("pkg reader parses a finalized package", "[pkg]") {
    const auto p = build_test_pkg();
    auto reader = pkg::ps3_pkg_reader::from_memory(p.bytes);

    REQUIRE(reader.finalized());
    REQUIRE(reader.mode() == pkg::crypto_mode::aes_ctr);
    REQUIRE(reader.content_id() == "UP0001-TEST12345_00-0000000000000000");
    REQUIRE(reader.title_id() == "TEST12345");
    REQUIRE(reader.entries().size() == 2);
    REQUIRE(reader.entries()[0].name == "PARAM.SFO");
    REQUIRE(reader.entries()[1].name == "USRDIR/EBOOT.BIN");
    REQUIRE(reader.entries()[1].data_size == 40000);
}

TEST_CASE("pkg reader decrypts entry data", "[pkg]") {
    const auto p = build_test_pkg();
    auto reader = pkg::ps3_pkg_reader::from_memory(p.bytes);

    const auto& e = reader.entries()[0];
    std::vector<std::byte> out(e.data_size);
    reader.decrypt_range(e, 0, out);
    REQUIRE(out == p.param_sfo);
}

TEST_CASE("pkg installer lays a package out under game/TITLE_ID", "[pkg][install]") {
    const auto p = build_test_pkg();
    auto reader = pkg::ps3_pkg_reader::from_memory(p.bytes);

    auto disk = tuf::build_cg_image();
    fs::ufs2_filesystem filesystem(disk, 0);
    REQUIRE(filesystem.mount());
    fs::ufs2_writer writer(filesystem, disk);
    writer.set_clock(1000);
    tuf::bootstrap_root(writer);

    pkg::pkg_installer installer(filesystem, writer, reader);
    const std::string path = installer.install();
    REQUIRE(path == "game/TEST12345");

    // decrypted files landed in the right place and read back correctly!
    const auto sfo = filesystem.resolve_path("game/TEST12345/PARAM.SFO");
    REQUIRE(sfo.has_value());
    REQUIRE(filesystem.read_inode_data(*sfo) == p.param_sfo);

    const auto eboot = filesystem.resolve_path("game/TEST12345/USRDIR/EBOOT.BIN");
    REQUIRE(eboot.has_value());
    REQUIRE(filesystem.read_inode_data(*eboot) == p.eboot);
}


#include "test_helpers.h"
#include <ps3hdd_cryptodisk/encrypted_disk_source_cbc.h>
#include <memory>

TEST_CASE("writer round-trips through the CBC-encrypted disk source", "[pkg][install][crypto]") {
    auto plain = tuf::build_cg_image();
    auto backing = std::make_shared<th::memory_disk_source>(plain.store().size());
    const auto key = tuf::pattern(24, 55); // arbitrary 24 byte CBC key

    // store the plaintext UFS2 image encrypted as it would be on disk
    {
        cryptodisk::encrypted_disk_source_cbc enc(backing, th::cspan(key), /*bswap16=*/true);
        enc.write_bytes(0, th::cspan(plain.store()));
    }

    // the fs + writer operate through a decrypting view
    auto dec = std::make_shared<cryptodisk::encrypted_disk_source_cbc>(backing, th::cspan(key), true);
    fs::ufs2_filesystem filesystem(*dec, 0);
    REQUIRE(filesystem.mount());
    fs::ufs2_writer writer(filesystem, *dec);
    writer.set_clock(1000);
    tuf::bootstrap_root(writer);

    const std::uint64_t d = writer.create_directory(2, "NPEA00333");
    const auto content = tuf::pattern(50000, 9); // multiblock file
    const std::uint64_t ino = writer.write_file(d, "PARAM.SFO", content);
    writer.update_superblock();

    REQUIRE(filesystem.resolve_path_to_inode_number("NPEA00333/PARAM.SFO") == ino);
    REQUIRE(filesystem.read_inode_data(filesystem.read_inode(ino)) == content);
}
