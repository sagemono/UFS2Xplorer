#include "memory_disk_source.h"

#include <ps3hdd_crypto/be_io.h>
#include <ps3hdd_fs/ufs2_checker.h>
#include <ps3hdd_fs/ufs2_filesystem.h>
#include <ps3hdd_fs/ufs2_writer.h>

#include <catch2/catch_test_macros.hpp>

#include <cstring>
#include <vector>

using namespace ps3hdd::fs;
using ps3hdd::read_be_u32;
using ps3hdd::write_be_u32;

namespace {

// 16 KB blocks
// 2 KB fragments (8 frags/block)
constexpr int kFrag = 2048;
constexpr int kBlock = 16384;
constexpr int kFpb = kBlock / kFrag; // 8
constexpr int kIpg = 256;
constexpr int kFpg = 4096;
constexpr int kCblkno = 8;  // CG header at fragment 8 (byte 16384)
constexpr int kDblkno = 80; // first data block: after the 32 frag inode table (iblkno 48)
constexpr int kIblkno = 48; // inode table start
constexpr int kCgSize = 16384;
constexpr std::uint64_t kCgHeader = static_cast<std::uint64_t>(kCblkno) * kFrag; // 16384

// CG-header-relative bitmap layout.
constexpr int kIusedoff = 168;
constexpr int kFreeoff = 208;    // fpg/8 = 512 bytes -> 208..720
constexpr int kClusteroff = 720; // nclusterblks/8 = 64 bytes -> 720..784
constexpr int kClustersumoff = 784;
constexpr int kNclusterblks = kFpg / kFpb; // 512
constexpr int kInitInited = 64; // inopb = block/256 = 64; inode 0 needs no extend

void set_bit(std::vector<std::byte>& d, std::size_t base, int idx) {
    d[base + idx / 8] |= static_cast<std::byte>(1 << (idx % 8));
}

void write_superblock(std::vector<std::byte>& img, int ncg) {
    std::byte* b = img.data() + 65536;
    write_be_u32(b + 0x55C, superblock::magic_value);
    write_be_u32(b + 0x30, kBlock);
    write_be_u32(b + 0x34, kFrag);
    write_be_u32(b + 0xBC, kFpg);
    write_be_u32(b + 0xB8, kIpg);
    write_be_u32(b + 0x10, kIblkno); // fs_iblkno
    write_be_u32(b + 0x0C, kCblkno); // fs_cblkno
    write_be_u32(b + 0x14, kDblkno); // fs_dblkno
    write_be_u32(b + 0xA0, kCgSize); // fs_cgsize
    write_be_u32(b + 0x2C, static_cast<std::uint32_t>(ncg)); // fs_ncg
}

std::uint64_t cg_header_offset(int cg) {
    return static_cast<std::uint64_t>(static_cast<std::int64_t>(cg) * kFpg + kCblkno) * kFrag;
}

// write one fully free cylinder group header and bitmaps at its offset
void write_cg_header(std::vector<std::byte>& img, int cg) {
    const std::uint64_t base = cg_header_offset(cg);
    std::byte* h = img.data() + base;
    write_be_u32(h + 0x04, cylinder_group::magic_value);
    write_be_u32(h + 0x0C, static_cast<std::uint32_t>(cg)); // cg_cgx
    write_be_u32(h + 0x14, kFpg);
    const int nbfree = (kFpg - kDblkno) / kFpb;
    write_be_u32(h + 0x1C, static_cast<std::uint32_t>(nbfree));
    write_be_u32(h + 0x20, kIpg);
    write_be_u32(h + 0x24, 0);
    write_be_u32(h + 0x5C, kIusedoff);
    write_be_u32(h + 0x60, kFreeoff);
    write_be_u32(h + 0x68, kClustersumoff);
    write_be_u32(h + 0x6C, kClusteroff);
    write_be_u32(h + 0x70, kNclusterblks);
    write_be_u32(h + 0x78, kInitInited);

    std::vector<std::byte> raw(img.begin() + base, img.begin() + base + kCgSize);
    for (int f = kDblkno; f < kFpg; ++f) set_bit(raw, kFreeoff, f);
    for (int bl = kDblkno / kFpb; bl < kNclusterblks; ++bl) set_bit(raw, kClusteroff, bl);
    std::memcpy(img.data() + base, raw.data(), raw.size());
}

// one superblock + one fully free cg.
th::memory_disk_source build_cg_image() {
    th::memory_disk_source disk(512 * 1024);
    write_superblock(disk.store(), 1);
    write_cg_header(disk.store(), 0);
    return disk;
}

// two cylinder groups, so a file can span the CG boundary
th::memory_disk_source build_two_cg_image() {
    const std::size_t bytes = static_cast<std::size_t>(2) * kFpg * kFrag; // 16 MB
    th::memory_disk_source disk(bytes);
    write_superblock(disk.store(), 2);
    write_cg_header(disk.store(), 0);
    write_cg_header(disk.store(), 1);
    return disk;
}

std::int32_t cg_i32(const cylinder_group& cg, std::size_t off) {
    return static_cast<std::int32_t>(read_be_u32(cg.raw_data.data() + off));
}
bool cg_bit(const cylinder_group& cg, int base, int idx) {
    return (std::to_integer<int>(cg.raw_data[base + idx / 8]) & (1 << (idx % 8))) != 0;
}

struct fixture {
    th::memory_disk_source disk = build_cg_image();
    ufs2_filesystem fs{disk, 0};
    fixture() { REQUIRE(fs.mount()); }
};

} // namespace

TEST_CASE("find_free_inode returns the first free inode", "[ufs2][writer]") {
    fixture f;
    ufs2_writer w(f.fs, f.disk);
    auto& cg = w.read_cylinder_group(0);
    REQUIRE(cg.magic == cylinder_group::magic_value);
    REQUIRE(w.find_free_inode(cg) == 0);
}

TEST_CASE("mark_inode_used sets the bit and decrements free inodes", "[ufs2][writer]") {
    fixture f;
    ufs2_writer w(f.fs, f.disk);
    auto& cg = w.read_cylinder_group(0);
    REQUIRE(cg.free_inodes == kIpg);
    w.mark_inode_used(cg, 0);
    REQUIRE(cg.free_inodes == kIpg - 1);
    REQUIRE(cg_bit(cg, kIusedoff, 0));
    REQUIRE(w.find_free_inode(cg) == 1);
}

TEST_CASE("find_free_block_run returns an aligned contiguous run", "[ufs2][writer]") {
    fixture f;
    ufs2_writer w(f.fs, f.disk);
    auto& cg = w.read_cylinder_group(0);
    auto [start, blocks] = w.find_free_block_run(cg, kFpb, 10);
    REQUIRE(start == kDblkno);
    REQUIRE(blocks == 10);
}

TEST_CASE("find_free_fragments returns the first free fragment", "[ufs2][writer]") {
    fixture f;
    ufs2_writer w(f.fs, f.disk);
    auto& cg = w.read_cylinder_group(0);
    REQUIRE(w.find_free_fragments(cg, 3) == kDblkno);
}

TEST_CASE("mark_fragments_used on a full block updates counts", "[ufs2][writer]") {
    fixture f;
    ufs2_writer w(f.fs, f.disk);
    auto& cg = w.read_cylinder_group(0);
    const int nbfree0 = cg_i32(cg, 0x1C);

    w.mark_fragments_used(cg, kDblkno, kFpb); // consume one whole block
    REQUIRE(cg_i32(cg, 0x1C) == nbfree0 - 1);  // one fewer free block
    REQUIRE(cg_i32(cg, 0x24) == 0);            // no partial fragments left
    for (int i = 0; i < kFpb; ++i) REQUIRE_FALSE(cg_bit(cg, kFreeoff, kDblkno + i));
    // Cluster bit for this block cleared.
    REQUIRE_FALSE(cg_bit(cg, kClusteroff, kDblkno / kFpb));
}

TEST_CASE("mark_fragments_used partial block updates frsum and free frags", "[ufs2][writer]") {
    fixture f;
    ufs2_writer w(f.fs, f.disk);
    auto& cg = w.read_cylinder_group(0);
    const int nbfree0 = cg_i32(cg, 0x1C);

    w.mark_fragments_used(cg, kDblkno, 3);     // 3 of 8 fragments used, 5 remain free
    REQUIRE(cg_i32(cg, 0x1C) == nbfree0 - 1);  // the block is no longer a full free block
    REQUIRE(cg_i32(cg, 0x24) == 5);            // 5 free fragments now counted
    REQUIRE(cg_i32(cg, 0x34 + 5 * 4) == 1);    // frsum[5] incremented
}

TEST_CASE("mark_fragment_used single fragment", "[ufs2][writer]") {
    fixture f;
    ufs2_writer w(f.fs, f.disk);
    auto& cg = w.read_cylinder_group(0);
    const int nbfree0 = cg_i32(cg, 0x1C);

    w.mark_fragment_used(cg, kDblkno);
    REQUIRE(cg_i32(cg, 0x1C) == nbfree0 - 1);
    REQUIRE(cg_i32(cg, 0x24) == kFpb - 1);     // 7 fragments remain free
    REQUIRE(cg_i32(cg, 0x34 + (kFpb - 1) * 4) == 1); // frsum[7] incremented
}

TEST_CASE("write_cylinder_group persists via flush", "[ufs2][writer]") {
    fixture f;
    ufs2_writer w(f.fs, f.disk);
    auto& cg = w.read_cylinder_group(0);
    w.mark_inode_used(cg, 0);
    w.write_cylinder_group(cg);
    w.flush_dirty_cgs();

    // a fresh writer rereading the CG from disk sees the change
    ufs2_writer w2(f.fs, f.disk);
    auto& cg2 = w2.read_cylinder_group(0);
    REQUIRE(cg2.free_inodes == kIpg - 1);
    REQUIRE(cg_bit(cg2, kIusedoff, 0));
}

namespace {
// turn a freshly formatted CG image into one with a valid empty root directory (inode 2), using the writer primitives so the bitmaps stay consistent
void bootstrap_root(ufs2_writer& w) {
    auto& cg = w.read_cylinder_group(0);
    w.mark_inode_used(cg, 0); // reserved
    w.mark_inode_used(cg, 1); // reserved
    w.mark_inode_used(cg, 2); // root
    const int fpb = kFpb;
    const std::int64_t frag = w.find_free_fragments(cg, fpb); // a full block for root
    REQUIRE(frag >= 0);
    auto block = w.build_empty_directory_block(2, 2);
    w.write_data_block(frag, block);
    w.mark_fragments_used(cg, static_cast<int>(frag), fpb);
    auto inode = w.build_directory_inode(frag, /*nlink=*/2);
    w.write_inode(2, inode);
    w.write_cylinder_group(cg);
    w.update_superblock(); // flush 2 disk
}

std::vector<std::byte> pattern(std::size_t n, std::uint8_t seed) {
    std::vector<std::byte> v(n);
    for (std::size_t i = 0; i < n; ++i)
        v[i] = static_cast<std::byte>((i * 31 + seed) & 0xff);
    return v;
}
} // namespace

TEST_CASE("writer creates a directory the reader can list", "[ufs2][writer][roundtrip]") {
    auto disk = build_cg_image();
    ufs2_filesystem fs(disk, 0);
    REQUIRE(fs.mount());
    ufs2_writer w(fs, disk);
    w.set_clock(1000000);
    bootstrap_root(w);

    const std::uint64_t game = w.create_directory(2, "game");
    w.update_superblock();

    const auto root = fs.read_inode(2);
    auto entries = fs.read_directory(root);
    bool found = false;
    for (const auto& e : entries)
        if (e.name == "game") { found = true; REQUIRE(e.inode_number == game); }
    REQUIRE(found);
    REQUIRE(fs.resolve_path_to_inode_number("game") == game);

    // the new directory is itself valid: "." and "..".
    const auto gdir = fs.read_inode(game);
    REQUIRE(gdir.is_directory());
    auto gents = fs.read_directory(gdir);
    REQUIRE(gents.size() == 2);
    REQUIRE(gents[0].name == ".");
    REQUIRE(gents[1].name == "..");
}

TEST_CASE("writer stores a small file the reader reads back", "[ufs2][writer][roundtrip]") {
    auto disk = build_cg_image();
    ufs2_filesystem fs(disk, 0);
    REQUIRE(fs.mount());
    ufs2_writer w(fs, disk);
    w.set_clock(1000000);
    bootstrap_root(w);

    const auto content = pattern(5000, 7); // < one block: tail frag path
    const std::uint64_t ino = w.write_file(2, "PARAM.SFO", content);
    w.update_superblock();

    const auto num = fs.resolve_path_to_inode_number("PARAM.SFO");
    REQUIRE(num == ino);
    const auto file = fs.read_inode(ino);
    REQUIRE(file.type == file_type::regular_file);
    REQUIRE(file.size == 5000);
    REQUIRE(fs.read_inode_data(file) == content);
}

TEST_CASE("many sub-block files keep CG free counts consistent", "[ufs2][writer][roundtrip][fsck]") { //regression check
    auto disk = build_cg_image();
    ufs2_filesystem fs(disk, 0);
    REQUIRE(fs.mount());
    ufs2_writer w(fs, disk);
    w.set_clock(1000000);
    bootstrap_root(w);

    for (int i = 0; i < 40; ++i) {
        const std::size_t sz = 1500 + static_cast<std::size_t>((i * 733) % 6000); // subblock, unaligned
        w.write_file(2, "f" + std::to_string(i), pattern(sz, static_cast<std::uint8_t>(i)));
    }
    w.update_superblock();

    const auto rep = check_consistency(fs, disk);
    INFO("summary_mismatches=" << rep.summary_mismatches << " cross_links=" << rep.cross_links);
    REQUIRE(rep.summary_mismatches == 0);
    REQUIRE(rep.cross_links == 0);
}

TEST_CASE("writer stores a multi-block file", "[ufs2][writer][roundtrip]") {
    auto disk = build_cg_image();
    ufs2_filesystem fs(disk, 0);
    REQUIRE(fs.mount());
    ufs2_writer w(fs, disk);
    w.set_clock(1000000);
    bootstrap_root(w);

    const auto content = pattern(40000, 3); // 3 blocks: two full + a tail
    const std::uint64_t ino = w.write_file(2, "EBOOT.BIN", content);
    w.update_superblock();

    const auto file = fs.read_inode(ino);
    REQUIRE(file.size == 40000);
    REQUIRE(fs.read_inode_data(file) == content);
}

TEST_CASE("writer nests a file inside a created directory", "[ufs2][writer][roundtrip]") {
    auto disk = build_cg_image();
    ufs2_filesystem fs(disk, 0);
    REQUIRE(fs.mount());
    ufs2_writer w(fs, disk);
    w.set_clock(1000000);
    bootstrap_root(w);

    const std::uint64_t usrdir = w.create_directory(2, "USRDIR");
    const auto content = pattern(1234, 9);
    const std::uint64_t ino = w.write_file(usrdir, "ICON0.PNG", content);
    w.update_superblock();

    REQUIRE(fs.resolve_path_to_inode_number("USRDIR/ICON0.PNG") == ino);
    REQUIRE(fs.read_inode_data(fs.read_inode(ino)) == content);
}

TEST_CASE("writer rejects a duplicate name", "[ufs2][writer][roundtrip]") {
    auto disk = build_cg_image();
    ufs2_filesystem fs(disk, 0);
    REQUIRE(fs.mount());
    ufs2_writer w(fs, disk);
    w.set_clock(1000000);
    bootstrap_root(w);

    w.write_file(2, "dup", pattern(100, 1));
    REQUIRE_THROWS(w.write_file(2, "dup", pattern(100, 2)));
}

TEST_CASE("writer stores a file needing single-indirect blocks", "[ufs2][writer][roundtrip]") {
    auto disk = build_cg_image();
    ufs2_filesystem fs(disk, 0);
    REQUIRE(fs.mount());
    ufs2_writer w(fs, disk);
    w.set_clock(1000000);
    bootstrap_root(w);

    // 200000 bytes = 13 blocks (bsize 16384) > 12 direct blocks -> single indirect
    const auto content = pattern(200000, 5);
    const std::uint64_t ino = w.write_file(2, "BIG.DAT", content);
    w.update_superblock();

    const auto file = fs.read_inode(ino);
    REQUIRE(file.size == 200000);
    REQUIRE(file.indirect_block != 0);
    REQUIRE(file.double_indirect_block == 0);
    REQUIRE(fs.read_inode_data(file) == content);
}

TEST_CASE("writer grows a directory beyond one fragment", "[ufs2][writer][roundtrip]") {
    auto disk = build_cg_image();
    ufs2_filesystem fs(disk, 0);
    REQUIRE(fs.mount());
    ufs2_writer w(fs, disk);
    w.set_clock(1000000);
    bootstrap_root(w);

    const std::uint64_t dir = w.create_directory(2, "USRDIR");
    std::vector<std::string> names;
    for (int i = 0; i < 100; ++i) {
        std::string nm = "file" + std::to_string(i) + ".bin";
        names.push_back(nm);
        w.write_file(dir, nm, pattern(40 + i, static_cast<std::uint8_t>(i)));
    }
    w.update_superblock();

    const auto d = fs.read_inode(dir);
    auto ents = fs.read_directory(d);
    int files = 0;
    for (const auto& e : ents)
        if (e.name != "." && e.name != "..") ++files;
    REQUIRE(files == 100);

    // every file is resolvable and reads back correctly!
    for (int i = 0; i < 100; ++i) {
        const auto num = fs.resolve_path_to_inode_number("USRDIR/" + names[i]);
        REQUIRE(num.has_value());
        REQUIRE(fs.read_inode_data(fs.read_inode(*num)) == pattern(40 + i, static_cast<std::uint8_t>(i)));
    }
}

TEST_CASE("writer spans a file across cylinder groups", "[ufs2][writer][roundtrip]") {
    auto disk = build_two_cg_image();
    ufs2_filesystem fs(disk, 0);
    REQUIRE(fs.mount());
    REQUIRE(fs.sb().cylinder_groups == 2);
    ufs2_writer w(fs, disk);
    w.set_clock(1000000);
    bootstrap_root(w);

    // consume most of CG 0 so the next file has to spill into CG 1
    auto& cg0 = w.read_cylinder_group(0);
    for (int k = 0; k < 490; ++k) {
        const std::int64_t f = w.find_free_fragments(cg0, kFpb);
        if (f < 0) break;
        w.mark_fragments_used(cg0, static_cast<int>(f), kFpb);
    }

    // ~480 KB (31 blocks) with only ~11 blocks left in CG 0 -> must span into CG 1
    const std::size_t sz = 30u * kBlock + 123;
    const auto content = pattern(sz, 77);
    const std::uint64_t ino = w.write_file(2, "BIGGAME.PKG", content);
    w.update_superblock();

    const auto file = fs.read_inode(ino);
    REQUIRE(file.size == static_cast<std::int64_t>(sz));
    // a block beyond CG 0 (abs frag >= kFpg) confirms the span here/
    bool spanned = false;
    for (int i = 0; i < 12; ++i)
        if (file.direct_blocks[i] >= kFpg) spanned = true;
    if (file.indirect_block >= kFpg) spanned = true;
    REQUIRE(spanned);
    REQUIRE(fs.read_inode_data(file) == content);
}

TEST_CASE("writer deletes a file and frees its space", "[ufs2][writer][delete]") {
    auto disk = build_cg_image();
    ufs2_filesystem fs(disk, 0);
    REQUIRE(fs.mount());
    ufs2_writer w(fs, disk);
    w.set_clock(1000000);
    bootstrap_root(w);

    const auto content = pattern(5000, 4);
    w.write_file(2, "TEMP.DAT", content);
    w.update_superblock();
    REQUIRE(fs.resolve_path_to_inode_number("TEMP.DAT").has_value());
    const long long free_before = fs.sb().free_space_bytes();

    // reread free space AFTER creating (baseline), anb then delete
    ufs2_writer w2(fs, disk);
    w2.set_clock(1000000);
    w2.delete_file(2, "TEMP.DAT");
    w2.update_superblock();

    REQUIRE_FALSE(fs.resolve_path_to_inode_number("TEMP.DAT").has_value());
    // deleting returns space so free space is no less than right after creation
    REQUIRE(fs.sb().free_space_bytes() >= free_before);
}

TEST_CASE("writer reuses a deleted file's inode and name", "[ufs2][writer][delete]") {
    auto disk = build_cg_image();
    ufs2_filesystem fs(disk, 0);
    REQUIRE(fs.mount());
    ufs2_writer w(fs, disk);
    w.set_clock(1000000);
    bootstrap_root(w);

    w.write_file(2, "A.BIN", pattern(100, 1));
    w.delete_file(2, "A.BIN");
    // the name is free again, now writing it must succeed and read back
    const auto content = pattern(9000, 2);
    const std::uint64_t ino = w.write_file(2, "A.BIN", content);
    w.update_superblock();
    REQUIRE(fs.resolve_path_to_inode_number("A.BIN") == ino);
    REQUIRE(fs.read_inode_data(fs.read_inode(ino)) == content);
}

TEST_CASE("writer deletes an empty directory but not a full one", "[ufs2][writer][delete]") {
    auto disk = build_cg_image();
    ufs2_filesystem fs(disk, 0);
    REQUIRE(fs.mount());
    ufs2_writer w(fs, disk);
    w.set_clock(1000000);
    bootstrap_root(w);

    const std::uint64_t dir = w.create_directory(2, "EMPTY");
    (void)dir;
    const std::uint64_t full = w.create_directory(2, "FULL");
    w.write_file(full, "inside.txt", pattern(50, 3));

    REQUIRE_THROWS(w.delete_directory(2, "FULL")); // not empty
    w.delete_directory(2, "EMPTY"); // ok
    w.update_superblock();

    REQUIRE_FALSE(fs.resolve_path_to_inode_number("EMPTY").has_value());
    REQUIRE(fs.resolve_path_to_inode_number("FULL").has_value());
}

TEST_CASE("writer deletes a single indirect file", "[ufs2][writer][delete]") {
    auto disk = build_cg_image();
    ufs2_filesystem fs(disk, 0);
    REQUIRE(fs.mount());
    ufs2_writer w(fs, disk);
    w.set_clock(1000000);
    bootstrap_root(w);

    w.write_file(2, "BIG.DAT", pattern(200000, 5)); // single indirect
    w.delete_file(2, "BIG.DAT");
    w.update_superblock();
    REQUIRE_FALSE(fs.resolve_path_to_inode_number("BIG.DAT").has_value());

    // the freed space is reusable for another big file.
    const auto content = pattern(200000, 6);
    const std::uint64_t ino = w.write_file(2, "BIG2.DAT", content);
    w.update_superblock();
    REQUIRE(fs.read_inode_data(fs.read_inode(ino)) == content);
}

TEST_CASE("writer renames a file in place", "[ufs2][writer][move]") {
    auto disk = build_cg_image();
    ufs2_filesystem fs(disk, 0);
    REQUIRE(fs.mount());
    ufs2_writer w(fs, disk);
    w.set_clock(1000000);
    bootstrap_root(w);

    const auto content = pattern(3000, 4);
    const std::uint64_t ino = w.write_file(2, "OLD.TMP", content);
    w.move_entry(2, "OLD.TMP", 2, "NEW.SFO");
    w.update_superblock();

    REQUIRE_FALSE(fs.resolve_path_to_inode_number("OLD.TMP").has_value());
    REQUIRE(fs.resolve_path_to_inode_number("NEW.SFO") == ino);
    REQUIRE(fs.read_inode_data(fs.read_inode(ino)) == content);
}

TEST_CASE("writer moves a file into a subdirectory", "[ufs2][writer][move]") {
    auto disk = build_cg_image();
    ufs2_filesystem fs(disk, 0);
    REQUIRE(fs.mount());
    ufs2_writer w(fs, disk);
    w.set_clock(1000000);
    bootstrap_root(w);

    const std::uint64_t usrdir = w.create_directory(2, "USRDIR");
    const auto content = pattern(9000, 5);
    const std::uint64_t ino = w.write_file(2, "EBOOT.BIN", content);
    w.move_entry(2, "EBOOT.BIN", usrdir, "EBOOT.BIN");
    w.update_superblock();

    REQUIRE_FALSE(fs.resolve_path_to_inode_number("EBOOT.BIN").has_value());
    REQUIRE(fs.resolve_path_to_inode_number("USRDIR/EBOOT.BIN") == ino);
    REQUIRE(fs.read_inode_data(fs.read_inode(ino)) == content);
}

TEST_CASE("writer moves a directory and updates its parent link", "[ufs2][writer][move]") {
    auto disk = build_cg_image();
    ufs2_filesystem fs(disk, 0);
    REQUIRE(fs.mount());
    ufs2_writer w(fs, disk);
    w.set_clock(1000000);
    bootstrap_root(w);

    const std::uint64_t a = w.create_directory(2, "A");
    const std::uint64_t b = w.create_directory(2, "B");
    w.write_file(a, "data.bin", pattern(200, 6));
    // move directory A (with its file) under B.
    w.move_entry(2, "A", b, "A");
    w.update_superblock();

    REQUIRE_FALSE(fs.resolve_path_to_inode_number("A").has_value());
    REQUIRE(fs.resolve_path_to_inode_number("B/A") == a);
    REQUIRE(fs.resolve_path_to_inode_number("B/A/data.bin").has_value());

    // the moved directory's ".." now points at B.
    const auto moved = fs.read_inode(a);
    auto ents = fs.read_directory(moved);
    REQUIRE(ents[1].name == "..");
    REQUIRE(ents[1].inode_number == b);
}

TEST_CASE("writer rejects a move onto an existing name", "[ufs2][writer][move]") {
    auto disk = build_cg_image();
    ufs2_filesystem fs(disk, 0);
    REQUIRE(fs.mount());
    ufs2_writer w(fs, disk);
    w.set_clock(1000000);
    bootstrap_root(w);

    w.write_file(2, "X", pattern(10, 1));
    w.write_file(2, "Y", pattern(10, 2));
    REQUIRE_THROWS(w.move_entry(2, "X", 2, "Y"));
}

TEST_CASE("adding to a single fragment directory ignores stale garbage in the block tail", "[ufs2][writer][regression]") {
    auto disk = build_cg_image();
    ufs2_filesystem fs(disk, 0);
    REQUIRE(fs.mount());
    ufs2_writer w(fs, disk);
    w.set_clock(1000000);
    bootstrap_root(w);
    auto& cg = w.read_cylinder_group(0);

    // craft directory inode 3 occupying exactly ONE fragment instead ot a full block
    w.mark_inode_used(cg, 3);
    const std::int64_t dfrag = w.find_free_fragments(cg, 1);
    REQUIRE(dfrag >= 0);
    REQUIRE(dfrag % kFpb == 0); // block aligned: the tail is frags dfrag+1..dfrag+kFpb-1

    // "." and ".." packed into the first 512 byte section of the single frag
    std::vector<std::byte> d(kFrag, std::byte{0});
    write_be_u32(d.data() + 0, 3);
    ps3hdd::write_be_u16(d.data() + 4, 12);
    d[6] = std::byte{4}; d[7] = std::byte{1}; d[8] = std::byte{'.'};
    write_be_u32(d.data() + 12, 2);
    ps3hdd::write_be_u16(d.data() + 16, 512 - 12);
    d[18] = std::byte{4}; d[19] = std::byte{2}; d[20] = std::byte{'.'}; d[21] = std::byte{'.'};
    for (int sec = 512; sec < kFrag; sec += 512) ps3hdd::write_be_u16(d.data() + sec + 4, 512);
    w.write_data_block(dfrag, d);
    w.mark_fragments_used(cg, static_cast<int>(dfrag), 1); // ONE fragment only

    // stale garbage in the unallocated tail: 
    // bytes that parse as "used" directory sections (nonzero inode + nonzero rec_len)
    // older version of the code used 2 scan these
    auto& store = disk.store();
    const std::size_t tail = static_cast<std::size_t>(dfrag + 1) * kFrag;
    for (std::size_t i = 0; i < static_cast<std::size_t>(kFpb - 1) * kFrag; ++i)
        store[tail + i] = std::byte{0xAB};

    // inode: di_size=512, di_blocks = a single fragment, one direct block
    auto inode = w.build_directory_inode(dfrag, /*nlink=*/2);
    ps3hdd::write_be_u64(inode.data() + 0x18, kFrag / 512); // di_blocks: single fragment
    w.write_inode(3, inode);
    w.write_cylinder_group(cg);

    // add an entry to the single fragment directory
    w.write_file(3, "FILE.BIN", pattern(1000, 5));

    // di_size must stay within the one allocated fragment, NOT inflate to a block
    const auto dir = fs.read_inode(3);
    REQUIRE(dir.size <= kFrag);

    // the entry is present and readable 
    bool found = false;
    for (const auto& e : fs.read_directory(dir))
        if (e.name == "FILE.BIN") found = true;
    REQUIRE(found);
}
