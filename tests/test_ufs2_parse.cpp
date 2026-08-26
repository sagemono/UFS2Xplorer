#include <ps3hdd_crypto/be_io.h>
#include <ps3hdd_fs/ufs2_types.h>

#include <catch2/catch_test_macros.hpp>

#include <cstring>
#include <vector>

using namespace ps3hdd::fs;
using ps3hdd::write_be_u16;
using ps3hdd::write_be_u32;
using ps3hdd::write_be_u64;

TEST_CASE("superblock parses key fields at the right offsets", "[ufs2][parse]") {
    std::vector<std::byte> b(8192, std::byte{0});
    write_be_u32(b.data() + 0x55C, superblock::magic_value);
    write_be_u32(b.data() + 0x30, 16384);    // fs_bsize
    write_be_u32(b.data() + 0x34, 2048);     // fs_fsize
    write_be_u32(b.data() + 0xBC, 4096);     // fs_fpg
    write_be_u32(b.data() + 0xB8, 256);      // fs_ipg
    write_be_u32(b.data() + 0x10, 48);       // fs_iblkno
    write_be_u32(b.data() + 0x2C, 3);        // fs_ncg
    write_be_u64(b.data() + 0x438, 1000000); // fs_size
    write_be_u64(b.data() + 0x440, 999000);  // fs_dsize
    write_be_u64(b.data() + 0x3F8, 500);     // cs_nbfree
    write_be_u64(b.data() + 0x408, 7);       // cs_nffree
    const char* name = "PS3-GAMEOS";
    std::memcpy(b.data() + 0x480, name, std::strlen(name));

    const auto sb = superblock::parse(b);
    REQUIRE(sb.valid());
    REQUIRE(sb.block_size == 16384);
    REQUIRE(sb.fragment_size == 2048);
    REQUIRE(sb.frags_per_group == 4096);
    REQUIRE(sb.inodes_per_group == 256);
    REQUIRE(sb.inode_block_offset == 48);
    REQUIRE(sb.cylinder_groups == 3);
    REQUIRE(sb.total_fragments == 1000000);
    REQUIRE(sb.free_blocks == 500);
    REQUIRE(sb.free_fragments == 7);
    REQUIRE(sb.volume_name == "PS3-GAMEOS");
    REQUIRE(sb.free_space_bytes() == 500 * 16384 + 7 * 2048);
}

TEST_CASE("bad magic yields an invalid superblock", "[ufs2][parse]") {
    std::vector<std::byte> b(8192, std::byte{0});
    write_be_u32(b.data() + 0x55C, 0xdeadbeef);
    REQUIRE_FALSE(superblock::parse(b).valid());
}

TEST_CASE("inode parses mode, size and block pointers", "[ufs2][parse]") {
    std::vector<std::byte> b(256, std::byte{0});
    write_be_u16(b.data() + 0x00, 0x8000 | 0644); // regular file, rw-r--r--
    write_be_u16(b.data() + 0x02, 1);             // nlink
    write_be_u32(b.data() + 0x04, 0);             // uid
    write_be_u64(b.data() + 0x10, 12345);         // size
    write_be_u64(b.data() + 0x28, 0x5f000000);    // mtime
    write_be_u64(b.data() + 0x70, 100);           // db[0]
    write_be_u64(b.data() + 0x78, 108);           // db[1]
    write_be_u64(b.data() + 0xD0, 5000);          // ib[0]

    const auto in = inode::parse(b, 3);
    REQUIRE(in.number == 3);
    REQUIRE(in.type == file_type::regular_file);
    REQUIRE_FALSE(in.is_directory());
    REQUIRE(in.size == 12345);
    REQUIRE(in.mtime == 0x5f000000);
    REQUIRE(in.direct_blocks[0] == 100);
    REQUIRE(in.direct_blocks[1] == 108);
    REQUIRE(in.indirect_block == 5000);
    REQUIRE(in.double_indirect_block == 0);
}

TEST_CASE("directory mode nibble maps to directory type", "[ufs2][parse]") {
    std::vector<std::byte> b(256, std::byte{0});
    write_be_u16(b.data() + 0x00, 0x4000 | 0755);
    REQUIRE(inode::parse(b, 2).type == file_type::directory);
}

namespace {
// append one directory entry (ino, type, name) with 4 byte aligned record
void put_entry(std::vector<std::byte>& b, std::uint32_t ino, dirent_type t, const std::string& name) {
    const std::size_t base = b.size();
    const std::uint16_t rec = static_cast<std::uint16_t>((8 + name.size() + 3) & ~std::size_t{3});
    b.resize(base + rec, std::byte{0});
    write_be_u32(b.data() + base, ino);
    write_be_u16(b.data() + base + 4, rec);
    b[base + 6] = static_cast<std::byte>(t);
    b[base + 7] = static_cast<std::byte>(name.size());
    std::memcpy(b.data() + base + 8, name.data(), name.size());
}
}

TEST_CASE("directory entries iterate by record length", "[ufs2][parse]") {
    std::vector<std::byte> b;
    put_entry(b, 2, dirent_type::directory, ".");
    put_entry(b, 2, dirent_type::directory, "..");
    put_entry(b, 3, dirent_type::regular_file, "hello.txt");
    put_entry(b, 4, dirent_type::directory, "USRDIR");
    b.resize(b.size() + 16, std::byte{0}); // trailing zeros -> parse stops

    const auto entries = parse_directory(b);
    REQUIRE(entries.size() == 4);
    REQUIRE(entries[0].name == ".");
    REQUIRE(entries[2].name == "hello.txt");
    REQUIRE(entries[2].inode_number == 3);
    REQUIRE(entries[2].type == dirent_type::regular_file);
    REQUIRE(entries[3].name == "USRDIR");
}