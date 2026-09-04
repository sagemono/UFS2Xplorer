#include <catch2/catch_test_macros.hpp>

#include "ufs2_real_geometry.h"

#include <ps3hdd_fs/ufs2_checker.h>
#include <ps3hdd_fs/ufs2_filesystem.h>
#include <ps3hdd_fs/ufs2_writer.h>

#include <string>

using namespace ps3hdd::fs;

namespace {
std::string dump(const consistency_report& r) {
    std::string s;
    for (const auto& f : r.findings) s += "\n  " + f;
    return s;
}
} // namespace

TEST_CASE("real geometry: image mounts and matches the drive", "[realgeom]") {
    auto disk = trg::build_real_geometry_image();
    ufs2_filesystem fs(disk, 0);
    REQUIRE(fs.mount());
    const auto& sb = fs.sb();
    REQUIRE(sb.cylinder_groups == trg::kNcg);
    REQUIRE(sb.inodes_per_group == trg::kIpg);
    REQUIRE(sb.frags_per_group == trg::kFpg);
    REQUIRE(sb.block_size == trg::kBlock);
    REQUIRE(sb.fragment_size == trg::kFrag);
    REQUIRE(sb.frag == trg::kFpb);
    REQUIRE(sb.cs_address == trg::kDblkno);
    REQUIRE(sb.total_fragments == trg::kTotalFrags);
}

TEST_CASE("real geometry: legacy allocator stays consistent", "[realgeom]") {
    auto disk = trg::build_real_geometry_image();
    ufs2_filesystem fs(disk, 0);
    REQUIRE(fs.mount());
    ufs2_writer w(fs, disk);
    trg::bootstrap_root(w);

    const std::uint64_t game = w.create_directory(2, "game");
    const std::uint64_t title = w.create_directory(game, "NPEA00252");
    const std::vector<std::byte> payload(static_cast<std::size_t>(trg::kBlock) * 4, std::byte{0xAB});
    for (int i = 0; i < 8; ++i)
        w.write_file(title, "f" + std::to_string(i) + ".bin", {payload.data(), payload.size()});
    w.update_superblock();
    w.flush_dirty_cgs();

    ufs2_filesystem check(disk, 0);
    REQUIRE(check.mount());
    const auto rep = check_consistency(check, disk);
    INFO("findings:" << dump(rep));
    CHECK(rep.cross_links == 0);
    CHECK(rep.out_of_range == 0);
    CHECK(rep.used_but_free == 0);
    CHECK(rep.summary_mismatches == 0);
    CHECK(rep.orphan_inodes == 0);
    CHECK(rep.cs_array_mismatches == 0);
    CHECK(rep.cstotal_mismatches == 0);
}

TEST_CASE("real geometry: policy install must not free blocks of existing files", "[realgeom]") {
    auto disk = trg::build_real_geometry_image();
    ufs2_filesystem fs(disk, 0);
    REQUIRE(fs.mount());
    ufs2_writer w(fs, disk);
    trg::bootstrap_root(w);

    const std::vector<std::byte> old_payload(static_cast<std::size_t>(trg::kBlock) * 3, std::byte{0x11});
    const std::uint64_t existing = w.create_directory(2, "existing");
    for (int i = 0; i < 60; ++i)
        w.write_file(existing, "old" + std::to_string(i) + ".bin", {old_payload.data(), old_payload.size()});
    w.update_superblock();
    w.flush_dirty_cgs();
    {
        ufs2_filesystem pre(disk, 0);
        REQUIRE(pre.mount());
        const auto r0 = check_consistency(pre, disk);
        INFO("pre-install findings:" << dump(r0));
        REQUIRE(r0.used_but_free == 0);
        REQUIRE(r0.cross_links == 0);
    }

    // Now the policy install on top of it.
    ufs2_writer w2(fs, disk);
    REQUIRE(w2.set_lv2_policy(true));
    const std::uint64_t game = w2.create_directory(2, "game");
    const std::uint64_t title = w2.create_directory(game, "NPEA00252");
    const std::vector<std::byte> payload(static_cast<std::size_t>(trg::kBlock) * 4, std::byte{0xCD});
    std::uint64_t parent = title;
    for (int d = 0; d < 16; ++d) {
        const std::uint64_t dir = w2.create_directory(parent, "dir" + std::to_string(d));
        for (int i = 0; i < 4; ++i)
            w2.write_file(dir, "f" + std::to_string(i) + ".bin", {payload.data(), payload.size()});
        parent = dir;
    }
    w2.update_superblock();
    w2.flush_dirty_cgs();

    ufs2_filesystem check(disk, 0);
    REQUIRE(check.mount());
    const auto rep = check_consistency(check, disk);
    INFO("post-install findings:" << dump(rep));
    CHECK(rep.used_but_free == 0); // the hardware failure signature, usually tr,ips up if somethings fucked
    CHECK(rep.cross_links == 0);
    CHECK(rep.orphan_inodes == 0);
    CHECK(rep.summary_mismatches == 0);
    CHECK(rep.cs_array_mismatches == 0);
    CHECK(rep.cstotal_mismatches == 0);
}

TEST_CASE("real geometry: lv2 policy install stays consistent", "[realgeom]") {
    auto disk = trg::build_real_geometry_image();
    ufs2_filesystem fs(disk, 0);
    REQUIRE(fs.mount());
    ufs2_writer w(fs, disk);
    REQUIRE(w.set_lv2_policy(true));
    trg::bootstrap_root(w);

    const std::uint64_t game = w.create_directory(2, "game");
    const std::uint64_t title = w.create_directory(game, "NPEA00252");
    const std::vector<std::byte> payload(static_cast<std::size_t>(trg::kBlock) * 4, std::byte{0xCD});
    std::uint64_t parent = title;
    for (int d = 0; d < 16; ++d) {
        const std::uint64_t dir = w.create_directory(parent, "dir" + std::to_string(d));
        for (int i = 0; i < 4; ++i)
            w.write_file(dir, "f" + std::to_string(i) + ".bin", {payload.data(), payload.size()});
        parent = dir;
    }
    w.update_superblock();
    w.flush_dirty_cgs();

    ufs2_filesystem check(disk, 0);
    REQUIRE(check.mount());
    const auto rep = check_consistency(check, disk);
    INFO("resident MB: " << (disk.resident_bytes() / (1024 * 1024)) << " findings:" << dump(rep));
    CHECK(rep.cross_links == 0);
    CHECK(rep.out_of_range == 0);
    CHECK(rep.used_but_free == 0);
    CHECK(rep.summary_mismatches == 0);
    CHECK(rep.orphan_inodes == 0);
    CHECK(rep.cs_array_mismatches == 0);
    CHECK(rep.cstotal_mismatches == 0);
}

TEST_CASE("real geometry: repair fixes used-but-free without losing data", "[realgeom]") {
    auto disk = trg::build_real_geometry_image();
    ufs2_filesystem fs(disk, 0);
    REQUIRE(fs.mount());
    ufs2_writer w(fs, disk);
    trg::bootstrap_root(w);

    const std::vector<std::byte> payload(static_cast<std::size_t>(trg::kBlock) * 3, std::byte{0x5A});
    const std::uint64_t dir = w.create_directory(2, "victim");
    const std::uint64_t ino = w.write_file(dir, "data.bin", {payload.data(), payload.size()});
    w.update_superblock();
    w.flush_dirty_cgs();

    {
        ufs2_filesystem f2(disk, 0);
        REQUIRE(f2.mount());
        const auto in = f2.read_inode(ino);
        const auto ptrs = f2.block_pointers(in);
        REQUIRE(!ptrs.empty());
        const int fpg = trg::kFpg;
        for (const auto p : ptrs) {
            const int cgn = static_cast<int>(p / fpg);
            const int base = static_cast<int>(p % fpg);
            const std::uint64_t off = trg::cg_header_offset(cgn);
            auto hdr = disk.read_bytes(off, trg::kCgSize);
            for (int k = 0; k < trg::kFpb; ++k) {
                const int f = base + k;
                hdr[static_cast<std::size_t>(trg::kFreeoff) + f / 8] |=
                    static_cast<std::byte>(1 << (f % 8)); // set = FREE
            }
            disk.write_bytes(off, {hdr.data(), hdr.size()});
        }
    }

    ufs2_filesystem bad(disk, 0);
    REQUIRE(bad.mount());
    const auto before = check_consistency(bad, disk);
    REQUIRE(before.used_but_free > 0);
    REQUIRE(before.used_but_free_frags.size() == static_cast<std::size_t>(before.used_but_free));

    {
        ufs2_writer w3(bad, disk);
        REQUIRE(w3.repair_used_but_free(before.used_but_free_frags) == before.used_but_free);
        w3.repair_free_counts({});
        w3.update_superblock();
        w3.flush_dirty_cgs();
    }

    ufs2_filesystem good(disk, 0);
    REQUIRE(good.mount());
    const auto after = check_consistency(good, disk);
    INFO("after repair:" << dump(after));
    CHECK(after.used_but_free == 0);
    CHECK(after.cross_links == 0);
    CHECK(after.summary_mismatches == 0);
    CHECK(after.cstotal_mismatches == 0);

    // and the file itself is untouched
    const auto in = good.read_inode(ino);
    const auto got = good.read_inode_data(in);
    REQUIRE(got.size() == payload.size());
    REQUIRE(std::equal(payload.begin(), payload.end(), got.begin()));
}

TEST_CASE("delete_tree releases inodes, not just blocks", "[realgeom][delete]") {
    auto disk = trg::build_real_geometry_image();
    ufs2_filesystem fs(disk, 0);
    REQUIRE(fs.mount());
    ufs2_writer w(fs, disk);
    trg::bootstrap_root(w);

    const std::vector<std::byte> payload(static_cast<std::size_t>(trg::kBlock) * 2, std::byte{0x77});
    const std::uint64_t game = w.create_directory(2, "game");
    const std::uint64_t title = w.create_directory(game, "NPEA00252");
    std::uint64_t parent = title;
    for (int d = 0; d < 6; ++d) {
        const std::uint64_t dir = w.create_directory(parent, "d" + std::to_string(d));
        for (int i = 0; i < 6; ++i)
            w.write_file(dir, "f" + std::to_string(i) + ".bin", {payload.data(), payload.size()});
        parent = dir;
    }
    w.update_superblock();
    w.flush_dirty_cgs();

    {
        ufs2_filesystem a(disk, 0);
        REQUIRE(a.mount());
        const auto r = check_consistency(a, disk);
        INFO("after install:" << dump(r));
        REQUIRE(r.orphan_inodes == 0);
        REQUIRE(r.used_but_free == 0);
    }
    {
        ufs2_filesystem b(disk, 0);
        REQUIRE(b.mount());
        ufs2_writer w2(b, disk);
        const auto gnum = b.resolve_path_to_inode_number("game");
        REQUIRE(gnum.has_value());
        w2.delete_tree(*gnum, "NPEA00252");
        w2.update_superblock();
        w2.flush_dirty_cgs();
    }

    ufs2_filesystem c(disk, 0);
    REQUIRE(c.mount());
    const auto rep = check_consistency(c, disk);
    INFO("after delete_tree:" << dump(rep));
    CHECK(rep.used_but_free == 0);
    CHECK(rep.cross_links == 0);
    CHECK(rep.summary_mismatches == 0);
    CHECK(rep.orphan_inodes == 0);
}

TEST_CASE("read_directory returns every entry of a large directory", "[realgeom][dir]") {
    auto disk = trg::build_real_geometry_image();
    ufs2_filesystem fs(disk, 0);
    REQUIRE(fs.mount());
    ufs2_writer w(fs, disk);
    trg::bootstrap_root(w);

    const std::uint64_t big = w.create_directory(2, "big");
    const std::vector<std::byte> tiny(64, std::byte{0x01});
    constexpr int kFiles = 400;
    for (int i = 0; i < kFiles; ++i)
        w.write_file(big, "file_with_a_longish_name_" + std::to_string(i) + ".bin", {tiny.data(), tiny.size()});
    w.update_superblock();
    w.flush_dirty_cgs();

    ufs2_filesystem c(disk, 0);
    REQUIRE(c.mount());
    const auto in = c.read_inode(big);
    const auto entries = c.read_directory(in);
    int named = 0;
    for (const auto& e : entries)
        if (e.name != "." && e.name != "..") ++named;
    INFO("directory size=" << in.size << " entries parsed=" << named);
    CHECK(named == kFiles);
}

TEST_CASE("delete_tree of a large directory leaves no orphans", "[realgeom][dir]") {
    auto disk = trg::build_real_geometry_image();
    ufs2_filesystem fs(disk, 0);
    REQUIRE(fs.mount());
    ufs2_writer w(fs, disk);
    trg::bootstrap_root(w);

    const std::uint64_t game = w.create_directory(2, "game");
    const std::uint64_t title = w.create_directory(game, "TEST12345");
    const std::vector<std::byte> tiny(64, std::byte{0x02});
    for (int i = 0; i < 400; ++i)
        w.write_file(title, "asset_" + std::to_string(i) + ".dat", {tiny.data(), tiny.size()});
    w.update_superblock();
    w.flush_dirty_cgs();

    {
        ufs2_filesystem a(disk, 0);
        REQUIRE(a.mount());
        const auto r = check_consistency(a, disk);
        REQUIRE(r.orphan_inodes == 0);
    }
    {
        ufs2_filesystem b(disk, 0);
        REQUIRE(b.mount());
        ufs2_writer w2(b, disk);
        const auto gnum = b.resolve_path_to_inode_number("game");
        REQUIRE(gnum.has_value());
        w2.delete_tree(*gnum, "TEST12345");
        w2.update_superblock();
        w2.flush_dirty_cgs();
    }

    ufs2_filesystem c(disk, 0);
    REQUIRE(c.mount());
    const auto rep = check_consistency(c, disk);
    INFO("after delete_tree of a 400-entry dir:" << dump(rep));
    CHECK(rep.orphan_inodes == 0);
    CHECK(rep.used_but_free == 0);
}

namespace {
std::uint64_t build_game_tree(ufs2_writer& w, std::uint64_t game) {
    const std::vector<std::byte> f(96, std::byte{0x33});
    auto files = [&](std::uint64_t d, const char* p, int n) {
        for (int i = 0; i < n; ++i)
            w.write_file(d, std::string(p) + std::to_string(i) + ".dat", {f.data(), f.size()});
    };
    const std::uint64_t title = w.create_directory(game, "NPEA00252");
    files(title, "root", 3);
    const std::uint64_t trop = w.create_directory(title, "TROPDIR");
    files(w.create_directory(trop, "NPWR00660_00"), "trp", 4);
    const std::uint64_t usr = w.create_directory(title, "USRDIR");
    files(usr, "u", 6);
    const std::uint64_t cache = w.create_directory(usr, "cache");
    const std::uint64_t psarc = w.create_directory(cache, "psarc");
    files(w.create_directory(psarc, "install1"), "i1_", 20);
    files(w.create_directory(psarc, "install2"), "i2_", 20);
    files(w.create_directory(w.create_directory(usr, "dialog"), "archive"), "ar", 30);
    const std::uint64_t movies = w.create_directory(usr, "movies");
    files(w.create_directory(movies, "cutscene"), "cs", 40);
    const std::uint64_t subs = w.create_directory(movies, "subtitles");
    for (const char* lang : {"british","chinese","dutch","english","french","german","italian", "japanese","korean","polish","portuguese","russian","spanish"}) 
    files(w.create_directory(subs, lang), "s", 22);
    files(w.create_directory(movies, "tv"), "tv", 44);
    files(w.create_directory(movies, "ui"), "mu", 68);
    const std::uint64_t sounds = w.create_directory(usr, "sounds");
    files(w.create_directory(sounds, "music"), "m", 90);
    const std::uint64_t grains = w.create_directory(sounds, "streaming_grains");
    for (int g = 0; g < 11; ++g) files(w.create_directory(grains, std::to_string(g)), "g", 8);
    const std::uint64_t xmb = w.create_directory(w.create_directory(usr, "ui"), "xmb");
    files(w.create_directory(xmb, "gamedata"), "gd", 3);
    files(w.create_directory(xmb, "savedata"), "sd", 3);
    return title;
}
} // namespace

TEST_CASE("delete_tree of a realistic game tree leaves no orphans", "[realgeom][dir]") {
    auto disk = trg::build_real_geometry_image();
    ufs2_filesystem fs(disk, 0);
    REQUIRE(fs.mount());
    ufs2_writer w(fs, disk);
    trg::bootstrap_root(w);

    const std::uint64_t game = w.create_directory(2, "game");
    build_game_tree(w, game);
    w.update_superblock();
    w.flush_dirty_cgs();

    std::int64_t before_inodes = 0;
    {
        ufs2_filesystem a(disk, 0);
        REQUIRE(a.mount());
        const auto r = check_consistency(a, disk);
        INFO("after install:" << dump(r));
        REQUIRE(r.orphan_inodes == 0);
        before_inodes = r.inodes_walked;
    }

    {
        ufs2_filesystem b(disk, 0);
        REQUIRE(b.mount());
        ufs2_writer w2(b, disk);
        const auto gnum = b.resolve_path_to_inode_number("game");
        REQUIRE(gnum.has_value());
        w2.delete_tree(*gnum, "NPEA00252");
        w2.update_superblock();
    }

    ufs2_filesystem c(disk, 0);
    REQUIRE(c.mount());
    const auto rep = check_consistency(c, disk);
    INFO("walked before=" << before_inodes << " after=" << rep.inodes_walked
         << " after delete_tree:" << dump(rep));
    CHECK(rep.orphan_inodes == 0);
    CHECK(rep.used_but_free == 0);
    CHECK(rep.cross_links == 0);
}

TEST_CASE("reclaim_orphan_inodes recovers leaked space safely", "[realgeom][orphan]") {
    auto disk = trg::build_real_geometry_image();
    ufs2_filesystem fs(disk, 0);
    REQUIRE(fs.mount());
    ufs2_writer w(fs, disk);
    trg::bootstrap_root(w);

    const std::vector<std::byte> keep(static_cast<std::size_t>(trg::kBlock) * 2, std::byte{0xEE});
    const std::uint64_t safe_dir = w.create_directory(2, "keepme");
    const std::uint64_t safe_file = w.write_file(safe_dir, "precious.bin", {keep.data(), keep.size()});
    const std::uint64_t game = w.create_directory(2, "game");
    build_game_tree(w, game);
    w.update_superblock();
    w.flush_dirty_cgs();

    {
        ufs2_filesystem b(disk, 0);
        REQUIRE(b.mount());
        const auto gino = b.resolve_path_to_inode_number("game");
        REQUIRE(gino.has_value());
        const auto gin = b.read_inode(*gino);
        const auto ptrs = b.block_pointers(gin);
        REQUIRE(!ptrs.empty());
        const std::uint64_t off = static_cast<std::uint64_t>(ptrs[0]) * trg::kFrag;
        auto blk = disk.read_bytes(off, trg::kBlock);
        std::size_t p = 0;
        bool cleared = false;
        while (p + 8 < blk.size()) {
            const std::uint16_t reclen = (std::to_integer<std::uint16_t>(blk[p + 4]) << 8) | std::to_integer<std::uint16_t>(blk[p + 5]);
            const std::uint8_t namlen = std::to_integer<std::uint8_t>(blk[p + 7]);
            if (reclen == 0) break;
            const std::string nm(reinterpret_cast<const char*>(blk.data() + p + 8), namlen);
            if (nm == "NPEA00252") {
                for (int k = 0; k < 4; ++k) blk[p + k] = std::byte{0}; // d_ino = 0
                cleared = true;
                break;
            }
            p += reclen;
        }
        REQUIRE(cleared);
        disk.write_bytes(off, {blk.data(), blk.size()});
    }

    ufs2_filesystem c(disk, 0);
    REQUIRE(c.mount());
    const std::int64_t free_while_leaked = c.sb().free_space_bytes();
    const auto orphaned = check_consistency(c, disk);
    REQUIRE(orphaned.orphan_inodes > 100);
    REQUIRE(orphaned.used_but_free == 0);
    REQUIRE(orphaned.orphan_all_inodes.size() == static_cast<std::size_t>(orphaned.orphan_inodes));

    CHECK_FALSE(orphaned.clean());
    CHECK(orphaned.safe_to_write());
    CHECK_FALSE(orphaned.structurally_damaged());
    CHECK_FALSE(orphaned.repairable());

    {
        ufs2_writer rw(c, disk);
        const auto claimed = claimed_fragment_map(c, disk);
        const int n = rw.reclaim_orphan_inodes(orphaned.orphan_all_inodes, claimed);
        CHECK(n == orphaned.orphan_inodes);
        rw.repair_free_counts({});
        rw.update_superblock();
    }

    ufs2_filesystem d(disk, 0);
    REQUIRE(d.mount());
    const auto after = check_consistency(d, disk);
    INFO("after reclaim:" << dump(after));
    CHECK(after.orphan_inodes == 0);
    CHECK(after.used_but_free == 0);
    CHECK(after.cross_links == 0);
    CHECK(after.summary_mismatches == 0);
    CHECK(after.cstotal_mismatches == 0);
    CHECK(d.sb().free_space_bytes() > free_while_leaked);

    const auto sf = d.read_inode(safe_file);
    const auto got = d.read_inode_data(sf);
    REQUIRE(got.size() == keep.size());
    REQUIRE(std::equal(keep.begin(), keep.end(), got.begin()));
    REQUIRE(d.resolve_path("/keepme/precious.bin").has_value());
}

TEST_CASE("reclaim must not free blocks a live file owns", "[realgeom][orphan]") {
    auto disk = trg::build_real_geometry_image();
    ufs2_filesystem fs(disk, 0);
    REQUIRE(fs.mount());
    ufs2_writer w(fs, disk);
    trg::bootstrap_root(w);

    const std::vector<std::byte> live(static_cast<std::size_t>(trg::kBlock) * 3, std::byte{0xA5});
    const std::uint64_t dir = w.create_directory(2, "live");
    const std::uint64_t live_ino = w.write_file(dir, "inuse.bin", {live.data(), live.size()});
    const std::uint64_t decoy_ino = w.write_file(dir, "decoy.bin", {live.data(), live.size()});
    w.update_superblock();
    w.flush_dirty_cgs();

    std::vector<std::int64_t> live_ptrs;
    {
        ufs2_filesystem a(disk, 0);
        REQUIRE(a.mount());
        live_ptrs = a.block_pointers(a.read_inode(live_ino));
        REQUIRE(live_ptrs.size() >= 3);

        auto raw = a.read_inode_raw(decoy_ino);
        for (std::size_t i = 0; i < 3; ++i)
            for (int b = 0; b < 8; ++b)
                raw[0x70 + i * 8 + b] = static_cast<std::byte>((live_ptrs[i] >> (56 - 8 * b)) & 0xFF);
        ufs2_writer w2(a, disk);
        w2.write_inode(decoy_ino, raw);

        const auto din = a.read_inode(dir);
        const auto dptrs = a.block_pointers(din);
        const std::uint64_t off = static_cast<std::uint64_t>(dptrs[0]) * trg::kFrag;
        auto blk = disk.read_bytes(off, trg::kBlock);
        std::size_t p = 0;
        bool cleared = false;
        while (p + 8 < blk.size()) {
            const std::uint16_t reclen = (std::to_integer<std::uint16_t>(blk[p + 4]) << 8) | std::to_integer<std::uint16_t>(blk[p + 5]);
            const std::uint8_t namlen = std::to_integer<std::uint8_t>(blk[p + 7]);
            if (reclen == 0) break;
            if (std::string(reinterpret_cast<const char*>(blk.data() + p + 8), namlen) == "decoy.bin") {
                for (int k = 0; k < 4; ++k) blk[p + k] = std::byte{0};
                cleared = true;
                break;
            }
            p += reclen;
        }
        REQUIRE(cleared);
        disk.write_bytes(off, {blk.data(), blk.size()});
        w2.flush_dirty_cgs();
    }

    ufs2_filesystem c(disk, 0);
    REQUIRE(c.mount());
    const auto rep = check_consistency(c, disk);
    REQUIRE(rep.orphan_inodes >= 1);

    {
        ufs2_writer rw(c, disk);
        const auto claimed = claimed_fragment_map(c, disk);
        rw.reclaim_orphan_inodes(rep.orphan_all_inodes, claimed);
        rw.repair_free_counts({});
        rw.update_superblock();
    }

    ufs2_filesystem d(disk, 0);
    REQUIRE(d.mount());
    const auto after = check_consistency(d, disk);
    INFO("after reclaim:" << dump(after));
    CHECK(after.orphan_inodes == 0);
    CHECK(after.used_but_free == 0);
    CHECK(after.cross_links == 0);

    const auto got = d.read_inode_data(d.read_inode(live_ino));
    REQUIRE(got.size() == live.size());
    REQUIRE(std::equal(live.begin(), live.end(), got.begin()));
}

TEST_CASE("cluster accounting stays correct across install and delete", "[realgeom][cluster]") {
    auto disk = trg::build_real_geometry_image();
    ufs2_filesystem fs(disk, 0);
    REQUIRE(fs.mount());
    ufs2_writer w(fs, disk);
    REQUIRE(w.set_lv2_policy(true));
    trg::bootstrap_root(w);

    const std::vector<std::byte> payload(static_cast<std::size_t>(trg::kBlock) * 5, std::byte{0x3C});
    const std::uint64_t d = w.create_directory(2, "clust");
    for (int i = 0; i < 12; ++i)
        w.write_file(d, "f" + std::to_string(i) + ".bin", {payload.data(), payload.size()});
    w.update_superblock();
    w.flush_dirty_cgs();
    {
        ufs2_filesystem a(disk, 0);
        REQUIRE(a.mount());
        const auto r = check_consistency(a, disk);
        INFO("after install:" << dump(r));
        CHECK(r.cluster_map_mismatches == 0);
        CHECK(r.cluster_sum_mismatches == 0);
    }

    for (int i = 0; i < 12; ++i) w.delete_file(d, "f" + std::to_string(i) + ".bin");
    w.update_superblock();
    w.flush_dirty_cgs();

    ufs2_filesystem c(disk, 0);
    REQUIRE(c.mount());
    const auto rep = check_consistency(c, disk);
    INFO("after delete:" << dump(rep));
    CHECK(rep.cluster_map_mismatches == 0);
    CHECK(rep.cluster_sum_mismatches == 0);
    CHECK(rep.summary_mismatches == 0);
    CHECK(rep.used_but_free == 0);
}

TEST_CASE("the checker catches corrupt cluster state and --fix repairs it", "[realgeom][cluster]") {
    auto disk = trg::build_real_geometry_image();
    ufs2_filesystem fs(disk, 0);
    REQUIRE(fs.mount());
    ufs2_writer w(fs, disk);
    trg::bootstrap_root(w);
    const std::vector<std::byte> payload(static_cast<std::size_t>(trg::kBlock) * 3, std::byte{0x5E});
    w.write_file(2, "x.bin", {payload.data(), payload.size()});
    w.update_superblock();
    w.flush_dirty_cgs();

    {
        const std::uint64_t off = trg::cg_header_offset(0);
        auto h = disk.read_bytes(off, trg::kCgSize);
        for (int b = 600; b < 900; ++b)
            h[static_cast<std::size_t>(trg::kClusteroff) + b / 8] &= static_cast<std::byte>(~(1 << (b % 8)));
        ps3hdd::write_be_u32(h.data() + trg::kClustersumoff + 4, 0xFFFFFFB0u); // clustersum[1] = -80
        disk.write_bytes(off, {h.data(), h.size()});
    }

    ufs2_filesystem bad(disk, 0);
    REQUIRE(bad.mount());
    const auto before = check_consistency(bad, disk);
    CHECK(before.cluster_map_mismatches > 0);
    CHECK(before.cluster_sum_mismatches > 0);
    CHECK_FALSE(before.clean());

    {
        ufs2_writer rw(bad, disk);
        rw.repair_free_counts({});
        rw.update_superblock();
        rw.flush_dirty_cgs();
    }

    ufs2_filesystem good(disk, 0);
    REQUIRE(good.mount());
    const auto after = check_consistency(good, disk);
    INFO("after repair:" << dump(after));
    CHECK(after.cluster_map_mismatches == 0);
    CHECK(after.cluster_sum_mismatches == 0);
    CHECK(after.clean());
}

TEST_CASE("report severity separates damage, repairable drift and leaked inodes", "[checker]") {
    consistency_report r;
    CHECK(r.clean());
    CHECK(r.safe_to_write());

    r = {}; r.orphan_inodes = 2004;
    CHECK_FALSE(r.clean());
    CHECK(r.safe_to_write());

    r = {}; r.cluster_sum_mismatches = 1;
    CHECK_FALSE(r.safe_to_write());
    CHECK(r.repairable());
    CHECK_FALSE(r.structurally_damaged());

    r = {}; r.summary_mismatches = 1;
    CHECK(r.repairable());
    r = {}; r.used_but_free = 1;
    CHECK(r.repairable());
    r = {}; r.cs_array_mismatches = 1;
    CHECK(r.repairable());
    r = {}; r.cstotal_mismatches = 1;
    CHECK(r.repairable());
    r = {}; r.cluster_map_mismatches = 1;
    CHECK(r.repairable());

    r = {}; r.cross_links = 1;
    CHECK(r.structurally_damaged());
    CHECK_FALSE(r.safe_to_write());
    r = {}; r.out_of_range = 1;
    CHECK(r.structurally_damaged());
    CHECK_FALSE(r.safe_to_write());
}

TEST_CASE("a directory boxed in by neighbouring allocations relocates instead of failing", "[realgeom][dir]") {
    auto disk = trg::build_real_geometry_image();
    {
        ufs2_filesystem fs(disk, 0);
        REQUIRE(fs.mount());
        ufs2_writer w(fs, disk);
        trg::bootstrap_root(w);
        const std::uint64_t dir = w.create_directory(2, "dlc");
        w.create_directory(dir, "e000");
        w.update_superblock();
        w.flush_dirty_cgs();
    }

    std::int64_t first_block = 0;
    {
        ufs2_filesystem b(disk, 0);
        REQUIRE(b.mount());
        const auto d = b.resolve_path_to_inode_number("dlc");
        REQUIRE(d.has_value());
        const auto ptrs = b.block_pointers(b.read_inode(*d));
        REQUIRE(!ptrs.empty());
        first_block = ptrs[0];
    }
    {
        ufs2_filesystem b(disk, 0);
        REQUIRE(b.mount());
        const auto d = b.resolve_path_to_inode_number("dlc");
        REQUIRE(d.has_value());
        const std::uint64_t ino = *d;
        const std::uint64_t off =
            static_cast<std::uint64_t>(ino / trg::kIpg) * trg::kFpg * trg::kFrag +
            static_cast<std::uint64_t>(trg::kIblkno) * trg::kFrag +
            static_cast<std::uint64_t>(ino % trg::kIpg) * 256;
        auto in = disk.read_bytes(off, 256);
        ps3hdd::write_be_u64(in.data() + 0x18, static_cast<std::uint64_t>(trg::kFrag / 512));
        disk.write_bytes(off, {in.data(), in.size()});
    }

    ufs2_filesystem fs2(disk, 0);
    REQUIRE(fs2.mount());
    ufs2_writer w2(fs2, disk);

    const std::string pad(190, 'x');
    for (int i = 1; i < 40; ++i) {
        char nm[32];
        std::snprintf(nm, sizeof nm, "e%03d_", i);
        const auto dir2 = fs2.resolve_path_to_inode_number("dlc");
        REQUIRE(dir2.has_value());
        REQUIRE_NOTHROW(w2.create_directory(*dir2, nm + pad));
    }
    w2.update_superblock();
    w2.flush_dirty_cgs();

    ufs2_filesystem re(disk, 0);
    REQUIRE(re.mount());
    const auto dino = re.resolve_path_to_inode_number("dlc");
    REQUIRE(dino.has_value());
    const auto din = re.read_inode(*dino);

    int count = 0;
    for (const auto& e : re.read_directory(din))
        if (e.name != "." && e.name != "..") ++count;
    CHECK(count == 40);

    const auto ptrs = re.block_pointers(din);
    REQUIRE(!ptrs.empty());
    CHECK(ptrs[0] != first_block);

    const auto rep = check_consistency(re, disk);
    INFO("after boxed in growth:" << dump(rep));
    CHECK(rep.cross_links == 0);
    CHECK(rep.used_but_free == 0);
    CHECK(rep.safe_to_write());
}

TEST_CASE("deleting an indirect file frees only its own tail fragments", "[realgeom][free]") {
    auto disk = trg::build_real_geometry_image();
    {
        ufs2_filesystem fs(disk, 0);
        REQUIRE(fs.mount());
        ufs2_writer w(fs, disk);
        trg::bootstrap_root(w);
        const std::int64_t big_size = 12 * trg::kBlock + 2 * trg::kFrag;
        const std::vector<std::byte> big(static_cast<std::size_t>(big_size), std::byte{0xAB});
        w.write_file(2, "big.bin", {big.data(), big.size()});
        w.update_superblock();
        w.flush_dirty_cgs();
    }

    std::int64_t tail = 0;
    {
        ufs2_filesystem b(disk, 0);
        REQUIRE(b.mount());
        const auto f = b.resolve_path("big.bin");
        REQUIRE(f.has_value());
        const auto ptrs = b.block_pointers(*f);
        REQUIRE(ptrs.size() == 13);
        tail = ptrs.back();
    }

    const int cgn = static_cast<int>(tail / trg::kFpg);
    const int base = static_cast<int>(tail % trg::kFpg);
    const std::uint64_t cgoff = trg::cg_header_offset(cgn);
    auto is_free = [&](int f) {
        auto hdr = disk.read_bytes(cgoff, trg::kCgSize);
        return (std::to_integer<int>(hdr[static_cast<std::size_t>(trg::kFreeoff) + f / 8]) >> (f % 8)) & 1;
    };
    {
        auto hdr = disk.read_bytes(cgoff, trg::kCgSize);
        for (int f = base + 2; f < base + 4; ++f)
            hdr[static_cast<std::size_t>(trg::kFreeoff) + f / 8] &= static_cast<std::byte>(~(1 << (f % 8)));
        disk.write_bytes(cgoff, {hdr.data(), hdr.size()});
    }
    REQUIRE_FALSE(is_free(base + 2));
    REQUIRE_FALSE(is_free(base + 3));

    ufs2_filesystem fs2(disk, 0);
    REQUIRE(fs2.mount());
    ufs2_writer w2(fs2, disk);
    w2.repair_free_counts({});
    w2.delete_tree(2, "big.bin");
    w2.update_superblock();
    w2.flush_dirty_cgs();

    CHECK(is_free(base));
    CHECK(is_free(base + 1));
    CHECK_FALSE(is_free(base + 2));
    CHECK_FALSE(is_free(base + 3));
}