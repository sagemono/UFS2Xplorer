#pragma once

#include "ufs2_types.h"

#include <cstdint>
#include <functional>
#include <vector>

namespace ps3hdd::fs {

struct cg_summary {
    std::int32_t num_dirs = 0;
    std::int32_t free_blocks = 0;
    std::int32_t free_inodes = 0;
    std::int32_t free_fragments = 0;
};

struct ffs_context {
    const superblock* sb = nullptr;
    std::vector<cg_summary> cs;
    std::vector<std::uint8_t> contig_dirs;
    std::int32_t cg_rotor = 0;

    int cylinder_groups() const { return sb ? sb->cylinder_groups : 0; }
    void reset(const superblock& s);
};

std::int64_t ffs_dirpref(const ffs_context& ctx, std::uint64_t parent_inode, bool is_root_child = false, std::uint32_t random_cg = 0);
std::int64_t ffs_blkpref_ufs2(ffs_context& ctx, std::uint64_t inode_number, std::int64_t lbn, int indx, const std::vector<std::int64_t>& bap);
std::int64_t ffs_blkpref_ufs2_prev(ffs_context& ctx, std::uint64_t inode_number, std::int64_t lbn, int indx, std::int64_t prev_block);
std::int64_t ffs_hashalloc(const ffs_context& ctx, int cg, std::int64_t pref, const std::function<std::int64_t(int cg, std::int64_t pref)>& allocator);

} // namespace ps3hdd::fs
