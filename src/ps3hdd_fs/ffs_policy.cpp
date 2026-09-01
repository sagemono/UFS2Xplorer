#include "ffs_policy.h"

#include <algorithm>

namespace ps3hdd::fs {

void ffs_context::reset(const superblock& s) {
    sb = &s;
    const std::size_t n = static_cast<std::size_t>(std::max(0, s.cylinder_groups));
    cs.assign(n, cg_summary{});
    contig_dirs.assign(n, 0);
    cg_rotor = 0;
}

namespace {

int ino_to_cg(const superblock& sb, std::uint64_t ino) {
    return sb.inodes_per_group > 0 ? static_cast<int>(ino / static_cast<std::uint64_t>(sb.inodes_per_group)) : 0;
}

std::int64_t cg_base(const superblock& sb, int cg) {
    return sb.frags_per_group * static_cast<std::int64_t>(cg);
}

} // namespace

std::int64_t ffs_dirpref(const ffs_context& ctx, std::uint64_t parent_inode, bool is_root_child, std::uint32_t random_cg) {
    if (!ctx.sb) return 0;
    const superblock& sb = *ctx.sb;
    const int ncg = sb.cylinder_groups;
    const std::int64_t ipg = sb.inodes_per_group;
    if (ncg <= 0 || ipg <= 0 || static_cast<int>(ctx.cs.size()) < ncg) return 0;

    const std::int64_t avg_ifree = sb.free_inodes / ncg;
    const std::int64_t avg_bfree = sb.free_blocks / ncg;
    const std::int64_t avg_ndir = sb.directories / ncg;

    if (is_root_child) {
        const int pref_cg = ncg > 0 ? static_cast<int>(random_cg % static_cast<std::uint32_t>(ncg)) : 0;
        int min_cg = pref_cg;
        std::int64_t min_ndir = ipg;
        auto consider = [&](int cg) {
            const cg_summary& c = ctx.cs[static_cast<std::size_t>(cg)];
            if (c.num_dirs < min_ndir && c.free_inodes >= avg_ifree && c.free_blocks >= avg_bfree) {
                min_cg = cg;
                min_ndir = c.num_dirs;
            }
        };
        for (int cg = pref_cg; cg < ncg; ++cg) consider(cg);
        for (int cg = 0; cg < pref_cg; ++cg) consider(cg);
        return ipg * min_cg;
    }

    std::int64_t max_ndir = std::min<std::int64_t>(avg_ndir + ipg / 16, ipg);
    std::int64_t min_ifree = avg_ifree - avg_ifree / 4;
    if (min_ifree < 1) min_ifree = 1;
    std::int64_t min_bfree = avg_bfree - avg_bfree / 4;
    if (min_bfree < 1) min_bfree = 1;

    const std::int64_t cg_size_bytes = static_cast<std::int64_t>(sb.fragment_size) * sb.frags_per_group;
    std::int64_t dir_size = static_cast<std::int64_t>(sb.avg_file_size) * sb.avg_files_per_dir;
    const std::int64_t cur_dir_size = avg_ndir ? (cg_size_bytes - avg_bfree * sb.block_size) / avg_ndir : 0;
    if (dir_size < cur_dir_size) dir_size = cur_dir_size;

    std::int64_t max_contig_dirs = 0;
    if (dir_size > 0) max_contig_dirs = std::min<std::int64_t>((avg_bfree * sb.block_size) / dir_size, 255);
    if (sb.avg_files_per_dir > 0)
        max_contig_dirs = std::min<std::int64_t>(max_contig_dirs, ipg / sb.avg_files_per_dir);
    if (max_contig_dirs == 0) max_contig_dirs = 1;

    const int pref_cg = ino_to_cg(sb, parent_inode) % ncg;

    auto fits = [&](int cg) {
        const cg_summary& c = ctx.cs[static_cast<std::size_t>(cg)];
        return c.num_dirs < max_ndir && c.free_inodes >= min_ifree && c.free_blocks >= min_bfree && ctx.contig_dirs[static_cast<std::size_t>(cg)] < max_contig_dirs;
    };
    for (int cg = pref_cg; cg < ncg; ++cg)
        if (fits(cg)) return ipg * cg;
    for (int cg = 0; cg < pref_cg; ++cg)
        if (fits(cg)) return ipg * cg;

    for (int cg = pref_cg; cg < ncg; ++cg)
        if (ctx.cs[static_cast<std::size_t>(cg)].free_inodes >= avg_ifree) return ipg * cg;
    for (int cg = 0; cg < pref_cg; ++cg)
        if (ctx.cs[static_cast<std::size_t>(cg)].free_inodes >= avg_ifree) return ipg * cg;

    return ipg * pref_cg;
}

std::int64_t ffs_blkpref_ufs2(ffs_context& ctx, std::uint64_t inode_number, std::int64_t lbn, int indx, const std::vector<std::int64_t>& bap) {
    const std::size_t i = static_cast<std::size_t>(indx);
    const std::int64_t prev = (indx > 0 && i - 1 < bap.size()) ? bap[i - 1] : 0;
    return ffs_blkpref_ufs2_prev(ctx, inode_number, lbn, indx, prev);
}

std::int64_t ffs_blkpref_ufs2_prev(ffs_context& ctx, std::uint64_t inode_number, std::int64_t lbn, int indx, std::int64_t prev_block) {
    if (!ctx.sb) return 0;
    const superblock& sb = *ctx.sb;
    const int ncg = sb.cylinder_groups;
    if (ncg <= 0 || static_cast<int>(ctx.cs.size()) < ncg) return 0;

    const std::int64_t frag = sb.frag;
    const std::int64_t maxbpg = sb.max_blocks_per_group;
    const std::int64_t ipg = sb.inodes_per_group;
    const std::int64_t fpg = sb.frags_per_group;

    if (maxbpg > 0 && (indx % maxbpg) != 0) {
        if (prev_block != 0) return prev_block + frag;
    }

    if (lbn < superblock::direct_blocks + sb.indirect_per_block) {
        const int cg = ino_to_cg(sb, inode_number);
        return cg_base(sb, cg) + frag;
    }

    std::int64_t start_cg;
    if (indx != 0 && prev_block != 0)
        start_cg = (fpg > 0 ? prev_block / fpg : 0) + 1; // dtog(bap[indx-1]) + 1
    else
        start_cg = (ipg > 0 ? static_cast<std::int64_t>(inode_number) / ipg : 0) + (maxbpg > 0 ? lbn / maxbpg : 0);
    start_cg %= ncg;

    const std::int64_t avg_bfree = sb.free_blocks / ncg;
    for (int cg = static_cast<int>(start_cg); cg < ncg; ++cg) {
        if (ctx.cs[static_cast<std::size_t>(cg)].free_blocks >= avg_bfree) {
            ctx.cg_rotor = cg;
            return cg_base(sb, cg) + frag;
        }
    }
    for (int cg = 0; cg <= static_cast<int>(start_cg) && cg < ncg; ++cg) {
        if (ctx.cs[static_cast<std::size_t>(cg)].free_blocks >= avg_bfree) {
            ctx.cg_rotor = cg;
            return cg_base(sb, cg) + frag;
        }
    }
    return 0;
}

std::int64_t ffs_hashalloc(const ffs_context& ctx, int cg, std::int64_t pref, const std::function<std::int64_t(int, std::int64_t)>& allocator) {
    if (!ctx.sb || !allocator) return 0;
    const int ncg = ctx.sb->cylinder_groups;
    if (ncg <= 0) return 0;

    if (std::int64_t r = allocator(cg, pref)) return r;

    int here = cg;
    for (int i = 1; i < ncg; i *= 2) {
        here += i;
        if (here >= ncg) here -= ncg;
        if (std::int64_t r = allocator(here, 0)) return r;
    }

    here = (cg + 2) % ncg;
    for (int i = 2; i < ncg; ++i) {
        if (std::int64_t r = allocator(here, 0)) return r;
        if (++here >= ncg) here = 0;
    }
    return 0;
}

} // namespace ps3hdd::fs