#include "memory_disk_source.h"

#include <ps3hdd_crypto/be_io.h>
#include <ps3hdd_fs/ufs2_filesystem.h>

#include <catch2/catch_test_macros.hpp>

#include <cstring>
#include <string>
#include <tuple>
#include <vector>

using namespace ps3hdd::fs;
using ps3hdd::write_be_u16;
using ps3hdd::write_be_u32;
using ps3hdd::write_be_u64;

namespace {

// minimal but self consistent UFS2 geometry for one gc
constexpr std::int64_t kFrag = 2048; // fs_fsize
constexpr std::int64_t kBlock = 2048; // fs_bsize (1 fragment per block)
constexpr std::int64_t kIpg = 256; // inodes per group
constexpr std::int64_t kFpg = 4096; // fragments per group
constexpr std::int64_t kIblkno = 48; // inode table starts here (fragments)
constexpr std::uint64_t kInodeTable = static_cast<std::uint64_t>(kIblkno) * kFrag; // 98304

std::uint64_t inode_off(std::uint64_t ino) { return kInodeTable + ino * superblock::inode_size; }
std::uint64_t frag_off(std::int64_t frag) { return static_cast<std::uint64_t>(frag) * kFrag; }

void write_superblock(std::vector<std::byte>& img) {
    std::byte* b = img.data() + 65536;
    write_be_u32(b + 0x55C, superblock::magic_value);
    write_be_u32(b + 0x30, static_cast<std::uint32_t>(kBlock));
    write_be_u32(b + 0x34, static_cast<std::uint32_t>(kFrag));
    write_be_u32(b + 0xBC, static_cast<std::uint32_t>(kFpg));
    write_be_u32(b + 0xB8, static_cast<std::uint32_t>(kIpg));
    write_be_u32(b + 0x10, static_cast<std::uint32_t>(kIblkno));
    write_be_u32(b + 0x2C, 1); // one cg
}

void write_inode(std::vector<std::byte>& img, std::uint64_t ino, std::uint16_t mode, std::int64_t size, std::int64_t db0) {
    std::byte* b = img.data() + inode_off(ino);
    write_be_u16(b + 0x00, mode);
    write_be_u16(b + 0x02, 1); // nlink
    write_be_u64(b + 0x10, static_cast<std::uint64_t>(size));
    write_be_u64(b + 0x70, static_cast<std::uint64_t>(db0)); // db[0]
}

std::int64_t write_dir_block(std::vector<std::byte>& img, std::int64_t frag, const std::vector<std::tuple<std::uint32_t, dirent_type, std::string>>& ents) {
    std::vector<std::byte> blk;
    for (const auto& [ino, t, name] : ents) {
        const std::size_t base = blk.size();
        const std::uint16_t rec = static_cast<std::uint16_t>((8 + name.size() + 3) & ~std::size_t{3});
        blk.resize(base + rec, std::byte{0});
        write_be_u32(blk.data() + base, ino);
        write_be_u16(blk.data() + base + 4, rec);
        blk[base + 6] = static_cast<std::byte>(t);
        blk[base + 7] = static_cast<std::byte>(name.size());
        std::memcpy(blk.data() + base + 8, name.data(), name.size());
    }
    std::memcpy(img.data() + frag_off(frag), blk.data(), blk.size());
    return static_cast<std::int64_t>(blk.size());
}

void write_file_block(std::vector<std::byte>& img, std::int64_t frag, const std::string& content) {
    std::memcpy(img.data() + frag_off(frag), content.data(), content.size());
}

// build test img 
th::memory_disk_source build_image() {
    th::memory_disk_source disk(256 * 1024);
    auto& img = disk.store();
    write_superblock(img);

    const std::string hello = "hi from ufs2\n";
    const std::string deep = "deep!";

    const std::int64_t root_used = write_dir_block(img, 100, {
        {2, dirent_type::directory, "."},
        {2, dirent_type::directory, ".."},
        {3, dirent_type::regular_file, "hello.txt"},
        {4, dirent_type::directory, "sub"},
    });
    write_inode(img, 2, 0x4000 | 0755, root_used, 100);

    write_file_block(img, 101, hello);
    write_inode(img, 3, 0x8000 | 0644, static_cast<std::int64_t>(hello.size()), 101);

    const std::int64_t sub_used = write_dir_block(img, 102, {
        {4, dirent_type::directory, "."},
        {2, dirent_type::directory, ".."},
        {5, dirent_type::regular_file, "deep.bin"},
    });
    write_inode(img, 4, 0x4000 | 0755, sub_used, 102);

    // /sub/deep.bin (inode 5) @ frag 103
    write_file_block(img, 103, deep);
    write_inode(img, 5, 0x8000 | 0644, static_cast<std::int64_t>(deep.size()), 103);

    return disk;
}

std::string to_string(const std::vector<std::byte>& v) {
    return std::string(reinterpret_cast<const char*>(v.data()), v.size());
}

} // namespace

TEST_CASE("mount reads the superblock", "[ufs2][fs]") {
    auto disk = build_image();
    ufs2_filesystem fs(disk, 0);
    REQUIRE(fs.mount());
    REQUIRE(fs.sb().block_size == kBlock);
    REQUIRE(fs.sb().inodes_per_group == kIpg);
}

TEST_CASE("read_directory lists the root entries", "[ufs2][fs]") {
    auto disk = build_image();
    ufs2_filesystem fs(disk, 0);
    REQUIRE(fs.mount());

    const auto root = fs.read_inode(ufs2_filesystem::root_inode);
    REQUIRE(root.is_directory());
    const auto entries = fs.read_directory(root);
    REQUIRE(entries.size() == 4);
    REQUIRE(entries[2].name == "hello.txt");
    REQUIRE(entries[3].name == "sub");
}

TEST_CASE("resolve_path walks nested directories", "[ufs2][fs]") {
    auto disk = build_image();
    ufs2_filesystem fs(disk, 0);
    REQUIRE(fs.mount());

    REQUIRE(fs.resolve_path_to_inode_number("/") == ufs2_filesystem::root_inode);
    REQUIRE(fs.resolve_path_to_inode_number("hello.txt") == 3);
    REQUIRE(fs.resolve_path_to_inode_number("/sub") == 4);
    REQUIRE(fs.resolve_path_to_inode_number("sub/deep.bin") == 5);
    REQUIRE_FALSE(fs.resolve_path_to_inode_number("sub/missing").has_value());
    REQUIRE_FALSE(fs.resolve_path_to_inode_number("hello.txt/x").has_value()); // not a directory
}

TEST_CASE("read file contents through the inode", "[ufs2][fs]") {
    auto disk = build_image();
    ufs2_filesystem fs(disk, 0);
    REQUIRE(fs.mount());

    const auto file = *fs.resolve_path("hello.txt");
    REQUIRE(file.type == file_type::regular_file);
    REQUIRE(file.size == 13);
    REQUIRE(to_string(fs.read_inode_data(file)) == "hi from ufs2\n");

    const auto deep = *fs.resolve_path("sub/deep.bin");
    REQUIRE(to_string(fs.read_inode_data(deep)) == "deep!");
}
