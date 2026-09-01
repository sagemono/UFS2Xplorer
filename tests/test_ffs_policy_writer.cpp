#include <catch2/catch_test_macros.hpp>

#include "ufs2_test_image.h"

#include <ps3hdd_fs/ufs2_checker.h>
#include <ps3hdd_fs/ufs2_filesystem.h>
#include <ps3hdd_fs/ufs2_writer.h>

#include <string>

using namespace ps3hdd::fs;

TEST_CASE("lv2 policy is off by default and the legacy allocator is untouched") {
    auto disk = tuf::build_two_cg_image();
    ufs2_filesystem fs(disk, 0);
    REQUIRE(fs.mount());
    ufs2_writer w(fs, disk);

    REQUIRE_FALSE(w.lv2_policy());
    tuf::bootstrap_root(w);
    const std::uint64_t ino = w.create_directory(2, "legacy");
    REQUIRE(ino / tuf::kIpg == 0);
}

TEST_CASE("lv2 policy refuses to enable without a cs summary array") {
    auto disk = tuf::build_two_cg_image();
    auto& store = disk.store();
    for (int i = 0; i < 8; ++i) store[65536 + 0x448 + i] = std::byte{0};
    for (int i = 0; i < 4; ++i) store[65536 + 0x9C + i] = std::byte{0};

    ufs2_filesystem fs(disk, 0);
    REQUIRE(fs.mount());
    ufs2_writer w(fs, disk);
    REQUIRE_FALSE(w.set_lv2_policy(true));
    REQUIRE_FALSE(w.lv2_policy());
}

TEST_CASE("lv2 policy loads the cs summary array off the disk") {
    auto disk = tuf::build_two_cg_image();
    ufs2_filesystem fs(disk, 0);
    REQUIRE(fs.mount());
    ufs2_writer w(fs, disk);

    REQUIRE(w.set_lv2_policy(true));
    const auto& ctx = w.policy();
    REQUIRE(ctx.cs.size() == 2);
    REQUIRE(ctx.contig_dirs.size() == 2);
    REQUIRE(ctx.cs[0].free_inodes == tuf::kIpg);
    REQUIRE(ctx.cs[1].free_inodes == tuf::kIpg);
    REQUIRE(ctx.cs[1].free_blocks == (tuf::kFpg - tuf::kDblkno) / tuf::kFpb);
    REQUIRE(ctx.cs[0].free_blocks == ctx.cs[1].free_blocks - 1);
    REQUIRE(ctx.contig_dirs[0] == 0);
    REQUIRE(ctx.contig_dirs[1] == 0);
}

TEST_CASE("lv2 policy keeps the cs summary in step with the bitmaps") {
    auto disk = tuf::build_two_cg_image();
    ufs2_filesystem fs(disk, 0);
    REQUIRE(fs.mount());
    ufs2_writer w(fs, disk);
    REQUIRE(w.set_lv2_policy(true));

    const std::int32_t before = w.policy().cs[0].free_inodes;
    tuf::bootstrap_root(w);
    REQUIRE(w.policy().cs[0].free_inodes < before);

    auto& cg0 = w.read_cylinder_group(0);
    REQUIRE(w.policy().cs[0].free_inodes == cg0.free_inodes);
    REQUIRE(w.policy().cs[0].free_blocks == cg0.free_blocks);
}

TEST_CASE("lv2 policy bumps contigdirs for the group a directory lands in") {
    auto disk = tuf::build_two_cg_image();
    ufs2_filesystem fs(disk, 0);
    REQUIRE(fs.mount());
    ufs2_writer w(fs, disk);
    REQUIRE(w.set_lv2_policy(true));
    tuf::bootstrap_root(w);

    const std::uint64_t ino = w.create_directory(2, "games");
    const std::size_t cg = static_cast<std::size_t>(ino / tuf::kIpg);
    REQUIRE(cg < w.policy().contig_dirs.size());
    REQUIRE(w.policy().contig_dirs[cg] == 1);

    const std::uint64_t ino2 = w.create_directory(ino, "child");
    const std::size_t cg2 = static_cast<std::size_t>(ino2 / tuf::kIpg);
    REQUIRE(w.policy().contig_dirs[cg2] >= 1);
}

TEST_CASE("lv2 policy passes the strengthened consistency checks across fresh groups") {
    auto disk = tuf::build_n_cg_image(8);
    ufs2_filesystem fs(disk, 0);
    REQUIRE(fs.mount());
    ufs2_writer w(fs, disk);
    REQUIRE(w.set_lv2_policy(true));
    tuf::bootstrap_root(w);

    const auto payload = tuf::pattern(static_cast<std::size_t>(tuf::kBlock) * 8, 5);
    std::uint64_t parent = 2;
    for (int i = 0; i < 12; ++i) {
        const std::uint64_t d = w.create_directory(parent, "dir" + std::to_string(i));
        w.write_file(d, "data.bin", {payload.data(), payload.size()});
        parent = d;
    }
    w.update_superblock();
    w.flush_dirty_cgs();

    ufs2_filesystem check(disk, 0);
    REQUIRE(check.mount());
    const auto rep = check_consistency(check, disk);
    INFO("findings: " << [&] { std::string s; for (const auto& f : rep.findings) s += "\n  " + f; return s; }());
    CHECK(rep.cross_links == 0);
    CHECK(rep.out_of_range == 0);
    CHECK(rep.used_but_free == 0);
    CHECK(rep.summary_mismatches == 0);
    CHECK(rep.orphan_inodes == 0);
    CHECK(rep.cs_array_mismatches == 0);
    CHECK(rep.cstotal_mismatches == 0);
}

TEST_CASE("lv2 policy lays a multi-block file out contiguously") {
    auto disk = tuf::build_two_cg_image();
    ufs2_filesystem fs(disk, 0);
    REQUIRE(fs.mount());
    ufs2_writer w(fs, disk);
    REQUIRE(w.set_lv2_policy(true));
    tuf::bootstrap_root(w);

    const auto payload = tuf::pattern(static_cast<std::size_t>(tuf::kBlock) * 6, 3);
    const std::uint64_t ino = w.write_file(2, "big.bin", {payload.data(), payload.size()});
    w.update_superblock();
    w.flush_dirty_cgs();

    ufs2_filesystem check(disk, 0);
    REQUIRE(check.mount());
    const auto in = check.read_inode(ino);
    const auto ptrs = check.block_pointers(in);
    REQUIRE(ptrs.size() >= 6);
    for (std::size_t i = 1; i < ptrs.size(); ++i)
        REQUIRE(ptrs[i] == ptrs[i - 1] + tuf::kFpb);
}

TEST_CASE("lv2 policy round-trips a file large enough to need indirect blocks") {
    auto disk = tuf::build_two_cg_image();
    ufs2_filesystem fs(disk, 0);
    REQUIRE(fs.mount());
    ufs2_writer w(fs, disk);
    REQUIRE(w.set_lv2_policy(true));
    tuf::bootstrap_root(w);

    const auto payload = tuf::pattern(static_cast<std::size_t>(tuf::kBlock) * 20 + 77, 9);
    const std::uint64_t ino = w.write_file(2, "indirect.bin", {payload.data(), payload.size()});
    w.update_superblock();
    w.flush_dirty_cgs();

    ufs2_filesystem check(disk, 0);
    REQUIRE(check.mount());
    const auto in = check.read_inode(ino);
    REQUIRE(in.indirect_block != 0);
    const auto got = check.read_inode_data(in);
    REQUIRE(got.size() == payload.size());
    REQUIRE(std::equal(payload.begin(), payload.end(), got.begin()));

    const auto rep = check_consistency(check, disk);
    REQUIRE(rep.cross_links == 0);
    REQUIRE(rep.out_of_range == 0);
    REQUIRE(rep.used_but_free == 0);
}

TEST_CASE("a tree built under the lv2 policy is still consistent and readable") {
    auto disk = tuf::build_two_cg_image();
    ufs2_filesystem fs(disk, 0);
    REQUIRE(fs.mount());
    ufs2_writer w(fs, disk);
    REQUIRE(w.set_lv2_policy(true));
    tuf::bootstrap_root(w);

    const std::uint64_t game = w.create_directory(2, "game");
    const std::uint64_t title = w.create_directory(game, "BLES01234");
    const std::uint64_t usrdir = w.create_directory(title, "USRDIR");
    const auto payload = tuf::pattern(40000, 7);
    w.write_file(title, "PARAM.SFO", {payload.data(), 1234});
    w.write_file(usrdir, "EBOOT.BIN", {payload.data(), payload.size()});
    w.update_superblock();
    w.flush_dirty_cgs();

    ufs2_filesystem check(disk, 0);
    REQUIRE(check.mount());
    const auto eboot = check.resolve_path("/game/BLES01234/USRDIR/EBOOT.BIN");
    REQUIRE(eboot.has_value());
    REQUIRE(eboot->size == static_cast<std::int64_t>(payload.size()));
    const auto got = check.read_inode_data(*eboot);
    REQUIRE(got.size() == payload.size());
    REQUIRE(std::equal(payload.begin(), payload.end(), got.begin()));

    const auto rep = check_consistency(check, disk);
    REQUIRE(rep.cross_links == 0);
    REQUIRE(rep.out_of_range == 0);
    REQUIRE(rep.used_but_free == 0);
}
TEST_CASE("lv2 policy honours and advances the cylinder-group rotors") {
    auto disk = tuf::build_n_cg_image(4);
    ufs2_filesystem fs(disk, 0);
    REQUIRE(fs.mount());
    ufs2_writer w(fs, disk);
    REQUIRE(w.set_lv2_policy(true));
    tuf::bootstrap_root(w);

    // Rotors advance as allocations happen, and are persisted in the cg header.
    const auto payload = tuf::pattern(static_cast<std::size_t>(tuf::kBlock) * 2, 4);
    w.write_file(2, "a.bin", {payload.data(), payload.size()});
    auto& cg0 = w.read_cylinder_group(0);
    const std::int32_t blk_rotor = cg0.block_rotor;
    const std::int32_t ino_rotor = cg0.inode_rotor;
    REQUIRE(blk_rotor > 0);
    REQUIRE(ino_rotor > 0);

    w.write_file(2, "b.bin", {payload.data(), payload.size()});
    auto& cg0b = w.read_cylinder_group(0);
    CHECK(cg0b.block_rotor > blk_rotor);
    CHECK(cg0b.inode_rotor > ino_rotor);
    w.update_superblock();
    w.flush_dirty_cgs();

    // and they survive a remount, i.e. they really went to disk
    ufs2_filesystem re(disk, 0);
    REQUIRE(re.mount());
    ufs2_writer w2(re, disk);
    auto& cg0c = w2.read_cylinder_group(0);
    CHECK(cg0c.block_rotor == cg0b.block_rotor);
    CHECK(cg0c.frag_rotor == cg0b.frag_rotor);
    CHECK(cg0c.inode_rotor == cg0b.inode_rotor);

    const auto rep = check_consistency(re, disk);
    CHECK(rep.used_but_free == 0);
    CHECK(rep.cross_links == 0);
    CHECK(rep.summary_mismatches == 0);
}

TEST_CASE("legacy allocator leaves the rotors alone") {
    auto disk = tuf::build_n_cg_image(4);
    ufs2_filesystem fs(disk, 0);
    REQUIRE(fs.mount());
    ufs2_writer w(fs, disk);
    REQUIRE_FALSE(w.lv2_policy());
    tuf::bootstrap_root(w);

    const auto payload = tuf::pattern(static_cast<std::size_t>(tuf::kBlock) * 2, 4);
    w.write_file(2, "a.bin", {payload.data(), payload.size()});
    auto& cg0 = w.read_cylinder_group(0);
    CHECK(cg0.block_rotor == 0);
    CHECK(cg0.frag_rotor == 0);
}

TEST_CASE("lv2 policy extends a directory fragment in place instead of failing") {
    auto disk = tuf::build_n_cg_image(4);
    ufs2_filesystem fs(disk, 0);
    REQUIRE(fs.mount());
    ufs2_writer w(fs, disk);
    REQUIRE(w.set_lv2_policy(true));
    tuf::bootstrap_root(w);

    // A directory starts life in a partial fragment run; adding enough entries used to
    // throw "fragment extension not implemented". ffs_fragextend grows it in place.
    const std::uint64_t d = w.create_directory(2, "many");
    for (int i = 0; i < 120; ++i)
        REQUIRE_NOTHROW(w.create_directory(d, "child_with_a_long_name_" + std::to_string(i)));
    w.update_superblock();
    w.flush_dirty_cgs();

    ufs2_filesystem c(disk, 0);
    REQUIRE(c.mount());
    const auto in = c.read_inode(d);
    int named = 0;
    for (const auto& e : c.read_directory(in))
        if (e.name != "." && e.name != "..") ++named;
    CHECK(named == 120);

    const auto rep = check_consistency(c, disk);
    INFO("findings: " << [&] { std::string s2; for (const auto& f : rep.findings) s2 += "\n  " + f; return s2; }());
    CHECK(rep.cross_links == 0);
    CHECK(rep.used_but_free == 0);
    CHECK(rep.summary_mismatches == 0);
}

TEST_CASE("lv2 policy stamps di_gen into freshly initialised inode blocks") {
    auto disk = tuf::build_n_cg_image(2);
    ufs2_filesystem fs(disk, 0);
    REQUIRE(fs.mount());
    ufs2_writer w(fs, disk);
    REQUIRE(w.set_lv2_policy(true));
    tuf::bootstrap_root(w);

    // Push past kInitInited so a new inode block gets initialised.
    const auto tiny = tuf::pattern(32, 1);
    std::uint64_t last = 0;
    for (int i = 0; i < 90; ++i)
        last = w.write_file(2, "f" + std::to_string(i) + ".bin", {tiny.data(), tiny.size()});
    w.update_superblock();
    w.flush_dirty_cgs();

    ufs2_filesystem c(disk, 0);
    REQUIRE(c.mount());
    // an inode in the newly initialised block that we never allocated should still carry
    // a non-zero di_gen, exactly as ffs_nodealloccg leaves it
    const auto raw = c.read_inode_raw(last + 3);
    const std::uint32_t gen = (std::to_integer<std::uint32_t>(raw[0x50]) << 24) |
                              (std::to_integer<std::uint32_t>(raw[0x51]) << 16) |
                              (std::to_integer<std::uint32_t>(raw[0x52]) << 8) |
                              std::to_integer<std::uint32_t>(raw[0x53]);
    CHECK(gen != 0);
}

TEST_CASE("lv2 policy shrinks a directory once its tail empties") {
    auto disk = tuf::build_n_cg_image(4);
    ufs2_filesystem fs(disk, 0);
    REQUIRE(fs.mount());
    ufs2_writer w(fs, disk);
    REQUIRE(w.set_lv2_policy(true));
    tuf::bootstrap_root(w);

    const std::uint64_t d = w.create_directory(2, "shrink");
    const auto tiny = tuf::pattern(16, 2);
    for (int i = 0; i < 400; ++i)
        w.write_file(d, "entry_with_a_long_name_" + std::to_string(i) + ".bin", {tiny.data(), tiny.size()});
    w.update_superblock();
    w.flush_dirty_cgs();

    std::int64_t size_before = 0;
    {
        ufs2_filesystem a(disk, 0);
        REQUIRE(a.mount());
        size_before = a.read_inode(d).size;
    }
    REQUIRE(size_before > tuf::kBlock);   // it really did grow past one block

    for (int i = 0; i < 400; ++i)
        w.delete_file(d, "entry_with_a_long_name_" + std::to_string(i) + ".bin");
    w.update_superblock();
    w.flush_dirty_cgs();

    ufs2_filesystem c(disk, 0);
    REQUIRE(c.mount());
    const auto in = c.read_inode(d);
    INFO("size before=" << size_before << " after=" << in.size);
    CHECK(in.size < size_before);         // the tail was released

    const auto rep = check_consistency(c, disk);
    CHECK(rep.cross_links == 0);
    CHECK(rep.used_but_free == 0);
    CHECK(rep.orphan_inodes == 0);
    CHECK(rep.summary_mismatches == 0);
}
