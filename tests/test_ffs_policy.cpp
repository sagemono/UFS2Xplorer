#include <catch2/catch_test_macros.hpp>

#include <ps3hdd_fs/ffs_policy.h>

using namespace ps3hdd::fs;

namespace {

superblock make_sb(int ncg = 8) {
    superblock sb;
    sb.magic = superblock::magic_value;
    sb.block_size = 16384;
    sb.fragment_size = 4096;
    sb.frag = 4;
    sb.cylinder_groups = ncg;
    sb.inodes_per_group = 1000;
    sb.frags_per_group = 20000;
    sb.max_blocks_per_group = 64;
    sb.indirect_per_block = 2048;// fs_nindir for a 16 kb block of 8b pointers
    sb.avg_file_size = 16384;
    sb.avg_files_per_dir = 64;
    return sb;
}

ffs_context make_ctx(const superblock& sb, std::int32_t ndir = 10, std::int32_t nbfree = 1000, std::int32_t nifree = 500) {
    ffs_context ctx;
    ctx.reset(sb);
    for (auto& c : ctx.cs) {
        c.num_dirs = ndir;
        c.free_blocks = nbfree;
        c.free_inodes = nifree;
    }
    return ctx;
}

void set_totals(superblock& sb, const ffs_context& ctx) {
    sb.directories = 0; sb.free_blocks = 0; sb.free_inodes = 0;
    for (const auto& c : ctx.cs) {
        sb.directories += c.num_dirs;
        sb.free_blocks += c.free_blocks;
        sb.free_inodes += c.free_inodes;
    }
}

} // namespace

TEST_CASE("ffs_dirpref keeps a subdirectory in its parent's cylinder group") {
    superblock sb = make_sb();
    ffs_context ctx = make_ctx(sb);
    set_totals(sb, ctx);
    ctx.sb = &sb;
    REQUIRE(ffs_dirpref(ctx, 3500) == 3 * sb.inodes_per_group);
}

TEST_CASE("ffs_dirpref skips a cylinder group that is over its contigdirs quota") {
    superblock sb = make_sb();
    ffs_context ctx = make_ctx(sb);
    set_totals(sb, ctx);
    ctx.sb = &sb;

    const std::int64_t in_place = ffs_dirpref(ctx, 3500);
    REQUIRE(in_place == 3 * sb.inodes_per_group);

    ctx.contig_dirs[3] = 255;
    REQUIRE(ffs_dirpref(ctx, 3500) == 4 * sb.inodes_per_group);
}

TEST_CASE("ffs_dirpref wraps around to the low groups") {
    superblock sb = make_sb();
    ffs_context ctx = make_ctx(sb);
    set_totals(sb, ctx);
    ctx.sb = &sb;
    ctx.contig_dirs[7] = 255;
    REQUIRE(ffs_dirpref(ctx, 7500) == 0);
}

TEST_CASE("ffs_dirpref root branch picks the group with the fewest directories") {
    superblock sb = make_sb();
    ffs_context ctx = make_ctx(sb);
    ctx.cs[5].num_dirs = 0; // clearly the emptiest
    set_totals(sb, ctx);
    ctx.sb = &sb;

    for (std::uint32_t draw = 0; draw < 8; ++draw)
        REQUIRE(ffs_dirpref(ctx, 2, /*is_root_child=*/true, draw) == 5 * sb.inodes_per_group);
}

TEST_CASE("ffs_blkpref_ufs2 lays blocks out contiguously") {
    superblock sb = make_sb();
    ffs_context ctx = make_ctx(sb);
    set_totals(sb, ctx);
    ctx.sb = &sb;

    const std::vector<std::int64_t> bap{1000, 1004, 1008};
    REQUIRE(ffs_blkpref_ufs2(ctx, 3500, 3, 3, bap) == 1008 + sb.frag);
}

TEST_CASE("ffs_blkpref_ufs2 keeps a small file in the inode's own cylinder group") {
    superblock sb = make_sb();
    ffs_context ctx = make_ctx(sb);
    set_totals(sb, ctx);
    ctx.sb = &sb;

    REQUIRE(ffs_blkpref_ufs2(ctx, 3500, 0, 0, {}) == 3 * sb.frags_per_group + sb.frag);
}

TEST_CASE("ffs_blkpref_ufs2 rotors to a group with above-average free space") {
    superblock sb = make_sb();
    ffs_context ctx = make_ctx(sb, 10, /*nbfree=*/10);
    ctx.cs[6].free_blocks = 100000; // the only group above average
    set_totals(sb, ctx);
    ctx.sb = &sb;

    const std::int64_t lbn = superblock::direct_blocks + sb.indirect_per_block + 1;
    REQUIRE(ffs_blkpref_ufs2(ctx, 500, lbn, 0, {}) == 6 * sb.frags_per_group + sb.frag);
    REQUIRE(ctx.cg_rotor == 6); // the kernel records where it stopped
}

TEST_CASE("ffs_hashalloc tries the preferred group first and passes the preference once") {
    superblock sb = make_sb();
    ffs_context ctx = make_ctx(sb);
    ctx.sb = &sb;

    std::vector<std::pair<int, std::int64_t>> calls;
    const std::int64_t got = ffs_hashalloc(ctx, 3, 4242, [&](int cg, std::int64_t pref) {
        calls.emplace_back(cg, pref);
        return cg == 3 ? 777 : 0;
    });
    REQUIRE(got == 777);
    REQUIRE(calls.size() == 1);
    REQUIRE(calls[0] == std::pair<int, std::int64_t>{3, 4242});
}

TEST_CASE("ffs_hashalloc falls back to the quadratic rehash, dropping the preference") {
    superblock sb = make_sb();
    ffs_context ctx = make_ctx(sb);
    ctx.sb = &sb;

    std::vector<std::pair<int, std::int64_t>> calls;
    const std::int64_t got = ffs_hashalloc(ctx, 0, 999, [&](int cg, std::int64_t pref) {
        calls.emplace_back(cg, pref);
        return cg == 3 ? 55 : 0;   // 0 -> +1 = 1 -> +2 = 3
    });
    REQUIRE(got == 55);
    REQUIRE(calls.size() == 3);
    REQUIRE(calls[0] == std::pair<int, std::int64_t>{0, 999});
    REQUIRE(calls[1] == std::pair<int, std::int64_t>{1, 0}); // retries never carry a pref
    REQUIRE(calls[2] == std::pair<int, std::int64_t>{3, 0});
}

TEST_CASE("ffs_hashalloc brute-forces every group before giving up") {
    superblock sb = make_sb();
    ffs_context ctx = make_ctx(sb);
    ctx.sb = &sb;

    int calls = 0;
    REQUIRE(ffs_hashalloc(ctx, 0, 0, [&](int, std::int64_t) { ++calls; return 0; }) == 0);
    // 1 preferred + 4 rehash steps (i = 1,2,4 wraps) + 6 bruteforce probe
    REQUIRE(calls > sb.cylinder_groups);
}
