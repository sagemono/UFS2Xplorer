#include "ufs2_writer.h"

#include <ps3hdd_crypto/be_io.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <optional>
#include <stdexcept>
#include <vector>

namespace ps3hdd::fs {

namespace {
constexpr std::size_t kMagic = 0x04;
constexpr std::size_t kNdblk = 0x14;
constexpr std::size_t kNbfree = 0x1C;
constexpr std::size_t kNifree = 0x20;
constexpr std::size_t kNffree = 0x24;
constexpr std::size_t kRotor = 0x28;  // cg_rotor  (wholeblock allocation)
constexpr std::size_t kFrotor = 0x2C; // cg_frotor (fragment searches)
constexpr std::size_t kIrotor = 0x30;
constexpr std::size_t kFrsum = 0x34; // cg_frsum[fragsPerBlock]
constexpr std::size_t kIusedoff = 0x5C;
constexpr std::size_t kFreeoff = 0x60;
constexpr std::size_t kClustersumoff = 0x68;
constexpr std::size_t kClusteroff = 0x6C;
constexpr std::size_t kNclusterblks = 0x70;
constexpr std::size_t kInitediblk = 0x78;

std::int32_t get_i32(const std::vector<std::byte>& d, std::size_t off) {
    return static_cast<std::int32_t>(ps3hdd::read_be_u32(d.data() + off));
}
void put_i32(std::vector<std::byte>& d, std::size_t off, std::int32_t v) {
    ps3hdd::write_be_u32(d.data() + off, static_cast<std::uint32_t>(v));
}
bool bit_set(const std::vector<std::byte>& d, std::size_t byte_idx, int bit) {
    return byte_idx < d.size() && (std::to_integer<int>(d[byte_idx]) & (1 << bit)) != 0;
}

std::int64_t dir_block_used_bytes(std::span<const std::byte> block) {
    constexpr int kDirBlkSiz = 512;
    const int sections = static_cast<int>(block.size()) / kDirBlkSiz;
    int last_used = -1;
    for (int s = 0; s < sections; ++s) {
        const int begin = s * kDirBlkSiz, end = begin + kDirBlkSiz;
        for (int o = begin; o + 8 <= end;) {
            const std::uint32_t ino = ps3hdd::read_be_u32(block.data() + o);
            const std::uint16_t rec = ps3hdd::read_be_u16(block.data() + o + 4);
            if (rec == 0) break;
            if (ino != 0) { last_used = s; break; }
            o += rec;
        }
    }
    return static_cast<std::int64_t>(last_used + 1) * kDirBlkSiz;
}
} // namespace

ufs2_writer::ufs2_writer(ufs2_filesystem& fs, disk::disk_source& disk)
    : fs_(fs), disk_(disk), sb_(fs.sb()) {}

cylinder_group& ufs2_writer::read_cylinder_group(int cg_number) {
    if (auto it = cg_cache_.find(cg_number); it != cg_cache_.end())
        return it->second;

    const std::uint64_t cg_offset = fs_.partition_offset_bytes() +
        static_cast<std::uint64_t>(cg_number) * sb_.frags_per_group * sb_.fragment_size;
    const std::uint64_t header_offset = cg_offset +
        static_cast<std::uint64_t>(sb_.cg_block_offset) * sb_.fragment_size;

    cylinder_group cg;
    cg.number = cg_number;
    cg.disk_offset = header_offset;
    cg.raw_data = disk_.read_bytes(header_offset, static_cast<std::size_t>(sb_.cg_size));

    cg.magic = ps3hdd::read_be_u32(cg.raw_data.data() + kMagic);
    cg.num_data_blocks = get_i32(cg.raw_data, kNdblk);
    cg.free_blocks = get_i32(cg.raw_data, kNbfree);
    cg.free_inodes = get_i32(cg.raw_data, kNifree);
    cg.inodes_used_offset = get_i32(cg.raw_data, kIusedoff);
    cg.free_blocks_offset = get_i32(cg.raw_data, kFreeoff);
    cg.block_rotor = get_i32(cg.raw_data, kRotor);
    cg.frag_rotor = get_i32(cg.raw_data, kFrotor);
    cg.inode_rotor = get_i32(cg.raw_data, kIrotor);
    cg.inited_iblk = get_i32(cg.raw_data, kInitediblk);

    auto [it, inserted] = cg_cache_.emplace(cg_number, std::move(cg));
    if (inserted) cg_summary_base_.emplace(cg_number, compute_cg_summary(it->second));
    return it->second;
}

std::array<std::int64_t, 5> ufs2_writer::compute_cg_summary(const cylinder_group& cg) const {
    if (cg.magic != cylinder_group::magic_value) return {0, 0, 0, 0, 0};
    const std::int64_t d = get_i32(cg.raw_data, 0x18), b = get_i32(cg.raw_data, 0x1C);
    const std::int64_t fi = get_i32(cg.raw_data, 0x20), ff = get_i32(cg.raw_data, 0x24);
    std::int64_t nclusters = 0;
    const int clusteroff = get_i32(cg.raw_data, 0x6C);
    const int nclusterblks = get_i32(cg.raw_data, 0x70);
    if (clusteroff > 0 && nclusterblks > 0)
        for (int bl = 0; bl < nclusterblks; ++bl)
            if (bit_set(cg.raw_data, static_cast<std::size_t>(clusteroff) + bl / 8, bl % 8)) ++nclusters;
    return {d, b, fi, ff, nclusters};
}

void ufs2_writer::write_cylinder_group(cylinder_group& cg) {
    dirty_cgs_.insert(cg.number);
    refresh_policy_summary(cg);
}

void ufs2_writer::refresh_policy_summary(const cylinder_group& cg) {
    if (!use_lv2_policy_) return;
    if (cg.number < 0 || static_cast<std::size_t>(cg.number) >= policy_.cs.size()) return;
    cg_summary& s = policy_.cs[static_cast<std::size_t>(cg.number)];
    s.num_dirs = get_i32(cg.raw_data, 0x18);
    s.free_blocks = get_i32(cg.raw_data, 0x1C);
    s.free_inodes = get_i32(cg.raw_data, 0x20);
    s.free_fragments = get_i32(cg.raw_data, 0x24);
}

bool ufs2_writer::load_policy_context() {
    policy_.reset(sb_);
    const int ncg = sb_.cylinder_groups;
    if (ncg <= 0 || sb_.cs_address <= 0 || sb_.cs_size <= 0 || sb_.fragment_size <= 0) return false;

    const std::size_t want = static_cast<std::size_t>(ncg) * 16;
    if (static_cast<std::size_t>(sb_.cs_size) < want) return false;
    std::vector<std::byte> raw;
    try {
        raw = disk_.read_bytes(fs_.partition_offset_bytes() + static_cast<std::uint64_t>(sb_.cs_address) * sb_.fragment_size, want);
    } catch (const std::exception&) {
        return false;
    }
    if (raw.size() < want) return false;
    for (int i = 0; i < ncg; ++i) {
        const std::size_t o = static_cast<std::size_t>(i) * 16;
        cg_summary& s = policy_.cs[static_cast<std::size_t>(i)];
        s.num_dirs = get_i32(raw, o + 0);
        s.free_blocks = get_i32(raw, o + 4);
        s.free_inodes = get_i32(raw, o + 8);
        s.free_fragments = get_i32(raw, o + 12);
    }
    return true;
}

bool ufs2_writer::set_lv2_policy(bool on) {
    if (!on) { use_lv2_policy_ = false; return false; }
    use_lv2_policy_ = load_policy_context();
    return use_lv2_policy_;
}

void ufs2_writer::flush_dirty_cgs() {
    for (int n : dirty_cgs_) {
        auto& cg = cg_cache_.at(n);
        disk_.write_bytes(cg.disk_offset, {cg.raw_data.data(), cg.raw_data.size()});
    }
    dirty_cgs_.clear();
}

int ufs2_writer::find_free_inode(const cylinder_group& cg) const {
    const int ipg = static_cast<int>(sb_.inodes_per_group);
    for (int i = 0; i < ipg; ++i) {
        const std::size_t byte_idx = static_cast<std::size_t>(cg.inodes_used_offset) + i / 8;
        if (byte_idx < cg.raw_data.size() && !bit_set(cg.raw_data, byte_idx, i % 8))
            return i;
    }
    return -1;
}

std::pair<std::int64_t, int> ufs2_writer::find_free_block_run(cylinder_group& cg, int fpb, int max_blocks) {
    const int fpg = static_cast<int>(sb_.frags_per_group);
    const int bitmap = cg.free_blocks_offset;

    int start_from = static_cast<int>(sb_.data_block_offset);
    if (auto it = cg_search_cursor_.find(cg.number); it != cg_search_cursor_.end() && it->second > start_from)
        start_from = it->second;
    if (start_from % fpb != 0) start_from = (start_from / fpb + 1) * fpb;

    int run_start = -1, run_blocks = 0;
    auto finish = [&](int rs, int rb) -> std::pair<std::int64_t, int> {
        cg_search_cursor_[cg.number] = rs + rb * fpb;
        return {rs, rb};
    };

    for (int f = start_from; f + fpb <= fpg; f += fpb) {
        bool block_free = true;
        for (int i = 0; i < fpb && block_free; ++i)
            block_free = bit_set(cg.raw_data, bitmap + (f + i) / 8, (f + i) % 8);

        if (block_free && !protected_fragments_.empty()) {
            const std::int64_t base = static_cast<std::int64_t>(cg.number) * sb_.frags_per_group + f;
            for (int i = 0; i < fpb; ++i)
                if (protected_fragments_.count(base + i)) { block_free = false; break; }
        }

        if (block_free) {
            if (run_start < 0) run_start = f;
            ++run_blocks;
            if (run_blocks >= max_blocks) return finish(run_start, run_blocks);
        } else if (run_blocks > 0) {
            return finish(run_start, run_blocks);
        } else {
            run_start = -1;
            run_blocks = 0;
        }
    }
    if (run_blocks > 0) return finish(run_start, run_blocks);
    return {-1, 0};
}

int ufs2_writer::rotor_start_for(const cylinder_group& cg, int count) const {
    const int r = (count >= frags_per_block()) ? cg.block_rotor : cg.frag_rotor;
    const int lo = static_cast<int>(sb_.data_block_offset);
    const int fpg = static_cast<int>(sb_.frags_per_group);
    if (r < lo || r >= fpg) return lo;
    return r;
}

void ufs2_writer::store_rotor(cylinder_group& cg, int count, int frag) {
    if (count >= frags_per_block()) {
        cg.block_rotor = frag;
        put_i32(cg.raw_data, kRotor, frag);
    } else {
        cg.frag_rotor = frag;
        put_i32(cg.raw_data, kFrotor, frag);
    }
}

int ufs2_writer::find_free_inode_rotor(cylinder_group& cg) {
    const int ipg = static_cast<int>(sb_.inodes_per_group);
    int start = cg.inode_rotor;
    if (start < 0 || start >= ipg) start = 0;
    for (int pass = 0; pass < 2; ++pass) {
        const int from = pass == 0 ? start : 0;
        const int to = pass == 0 ? ipg : start;
        for (int i = from; i < to; ++i) {
            const std::size_t byte_idx = static_cast<std::size_t>(cg.inodes_used_offset) + i / 8;
            if (byte_idx < cg.raw_data.size() && !bit_set(cg.raw_data, byte_idx, i % 8)) {
                cg.inode_rotor = i;
                put_i32(cg.raw_data, kIrotor, i);
                return i;
            }
        }
    }
    return -1;
}

std::int64_t ufs2_writer::find_free_fragments(cylinder_group& cg, int count, int pref_local) {
    const int fpg = static_cast<int>(sb_.frags_per_group);
    const int bitmap = cg.free_blocks_offset;
    const int fpb = frags_per_block();
    const bool require_align = count >= fpb;

    int start_from = static_cast<int>(sb_.data_block_offset);
    if (use_lv2_policy_) {
        start_from = (pref_local >= static_cast<int>(sb_.data_block_offset)) ? (pref_local / 8) * 8 : rotor_start_for(cg, count);
    } else if (auto it = cg_search_cursor_.find(cg.number); it != cg_search_cursor_.end() && it->second > start_from) {
        start_from = it->second;
    }
    if (require_align && start_from % fpb != 0) start_from = (start_from / fpb + 1) * fpb;

    int consecutive = 0, start_frag = -1;
    for (int f = start_from; f < fpg; ++f) {
        const bool is_free = bit_set(cg.raw_data, bitmap + f / 8, f % 8);
        const std::int64_t global = static_cast<std::int64_t>(cg.number) * sb_.frags_per_group + f;
        const bool blocked = protected_fragments_.count(global) != 0;

        if (is_free && !blocked) {
            if (consecutive == 0) {
                if (require_align && f % fpb != 0) { f = (f / fpb + 1) * fpb - 1; continue; }
                if (!require_align && (f % fpb) + count > fpb) {
                    f = (f / fpb + 1) * fpb - 1;
                    continue;
                }
                start_frag = f;
            }
            if (++consecutive == count) {
                cg_search_cursor_[cg.number] = start_frag + count;
                if (use_lv2_policy_) store_rotor(cg, count, start_frag);
                return start_frag;
            }
        } else {
            consecutive = 0;
            if (require_align) f = (f / fpb + 1) * fpb - 1;
        }
    }
    if (use_lv2_policy_ && start_from > static_cast<int>(sb_.data_block_offset)) {
        const int save = cg.block_rotor, savef = cg.frag_rotor;
        cg.block_rotor = cg.frag_rotor = static_cast<int>(sb_.data_block_offset);
        const std::int64_t r = find_free_fragments_scan(cg, count, static_cast<int>(sb_.data_block_offset), start_from);
        if (r < 0) { cg.block_rotor = save; cg.frag_rotor = savef; }
        return r;
    }
    return -1;
}

std::int64_t ufs2_writer::find_free_fragments_scan(cylinder_group& cg, int count, int from, int to) {
    const int fpb = frags_per_block();
    const int bitmap = cg.free_blocks_offset;
    const bool require_align = count >= fpb;
    int start = from;
    if (require_align && start % fpb != 0) start = (start / fpb + 1) * fpb;
    int consecutive = 0, start_frag = -1;
    for (int f = start; f < to; ++f) {
        const bool is_free = bit_set(cg.raw_data, bitmap + f / 8, f % 8);
        const std::int64_t global = static_cast<std::int64_t>(cg.number) * sb_.frags_per_group + f;
        if (is_free && protected_fragments_.count(global) == 0) {
            if (consecutive == 0) {
                if (require_align && f % fpb != 0) { f = (f / fpb + 1) * fpb - 1; continue; }
                if (!require_align && (f % fpb) + count > fpb) { f = (f / fpb + 1) * fpb - 1; continue; }
                start_frag = f;
            }
            if (++consecutive == count) {
                cg_search_cursor_[cg.number] = start_frag + count;
                store_rotor(cg, count, start_frag);
                return start_frag;
            }
        } else {
            consecutive = 0;
            if (require_align) f = (f / fpb + 1) * fpb - 1;
        }
    }
    return -1;
}

void ufs2_writer::mark_inode_used(cylinder_group& cg, int inode_idx) {
    if (inode_idx >= cg.inited_iblk) {
        int inopb = static_cast<int>(sb_.block_size / superblock::inode_size);
        if (inopb <= 0) inopb = 64;
        int new_inited = (inode_idx / inopb + 1) * inopb;
        if (new_inited > static_cast<int>(sb_.inodes_per_group))
            new_inited = static_cast<int>(sb_.inodes_per_group);

        const int old_blocks = cg.inited_iblk / inopb;
        const int new_blocks = new_inited / inopb;
        if (new_blocks - old_blocks > 0 && new_blocks - old_blocks < 1000) {
            std::vector<std::byte> zeros(static_cast<std::size_t>(sb_.block_size), std::byte{0});
            if (use_lv2_policy_) {
                for (std::size_t o = 0; o + superblock::inode_size <= zeros.size(); o += superblock::inode_size)
                    ps3hdd::write_be_u32(zeros.data() + o + 0x50, (next_gen() >> 1) + 1);
            }
            const std::int64_t cg_start = static_cast<std::int64_t>(cg.number) * sb_.frags_per_group;
            for (int blk = old_blocks; blk < new_blocks; ++blk) {
                const std::int64_t frag = cg_start + sb_.inode_block_offset +
                    static_cast<std::int64_t>(blk) * (sb_.block_size / sb_.fragment_size);
                const std::uint64_t off = fs_.partition_offset_bytes() +
                    static_cast<std::uint64_t>(frag) * sb_.fragment_size;
                disk_.write_bytes(off, {zeros.data(), zeros.size()});
            }
        }
        cg.inited_iblk = new_inited;
        put_i32(cg.raw_data, kInitediblk, cg.inited_iblk);
    }

    const std::size_t byte_idx = static_cast<std::size_t>(cg.inodes_used_offset) + inode_idx / 8;
    cg.raw_data[byte_idx] |= static_cast<std::byte>(1 << (inode_idx % 8));
    cg.free_inodes--;
    put_i32(cg.raw_data, kNifree, cg.free_inodes);
}

std::vector<int> ufs2_writer::block_free_runs(const cylinder_group& cg, int block_start, int fpb) const {
    std::vector<int> runs;
    int run = 0;
    for (int f = block_start; f < block_start + fpb; ++f) {
        if (bit_set(cg.raw_data, cg.free_blocks_offset + f / 8, f % 8)) {
            ++run;
        } else if (run > 0) {
            runs.push_back(run);
            run = 0;
        }
    }
    if (run > 0) runs.push_back(run);
    return runs;
}

void ufs2_writer::update_frsum(cylinder_group& cg, const std::vector<int>& old_runs, const std::vector<int>& new_runs, int fpb) {
    for (int r : old_runs)
        if (r > 0 && r < fpb) {
            const int v = get_i32(cg.raw_data, kFrsum + r * 4);
            if (v > 0) put_i32(cg.raw_data, kFrsum + r * 4, v - 1);
        }
    for (int r : new_runs)
        if (r > 0 && r < fpb)
            put_i32(cg.raw_data, kFrsum + r * 4, get_i32(cg.raw_data, kFrsum + r * 4) + 1);
}

void ufs2_writer::cluster_acct(cylinder_group& cg, int blkno, int cnt) {
    const int contigsum = sb_.contig_sum_size;
    if (contigsum <= 0) return;
    const int clusteroff = get_i32(cg.raw_data, kClusteroff);
    const int clustersumoff = get_i32(cg.raw_data, kClustersumoff);
    const int nclusterblks = get_i32(cg.raw_data, kNclusterblks);
    if (clusteroff == 0 || clustersumoff == 0 || nclusterblks == 0) return;
    if (blkno < 0 || blkno >= nclusterblks) return;

    auto isset = [&](int i) {
        const std::size_t b = static_cast<std::size_t>(clusteroff) + i / 8;
        return b < cg.raw_data.size() && ((std::to_integer<int>(cg.raw_data[b]) >> (i % 8)) & 1);
    };
    const std::size_t bit_byte = static_cast<std::size_t>(clusteroff) + blkno / 8;
    if (bit_byte >= cg.raw_data.size()) return;
    if (cnt > 0) cg.raw_data[bit_byte] |= static_cast<std::byte>(1 << (blkno % 8));
    else         cg.raw_data[bit_byte] &= static_cast<std::byte>(~(1 << (blkno % 8)));

    int end = blkno + 1 + contigsum;
    if (end > nclusterblks) end = nclusterblks;
    int forw = 0;
    for (int i = blkno + 1; i < end && isset(i); ++i) ++forw;
    int back = 0;
    for (int i = blkno - 1; i >= 0 && i > blkno - 1 - contigsum && isset(i); --i) ++back;

    auto sum_at = [&](int i) { return get_i32(cg.raw_data, static_cast<std::size_t>(clustersumoff) + 4 * i); };
    auto set_sum = [&](int i, std::int32_t v) { put_i32(cg.raw_data, static_cast<std::size_t>(clustersumoff) + 4 * i, v); };

    int i = back + forw + 1;
    if (i > contigsum) i = contigsum;
    set_sum(i, sum_at(i) + cnt);
    if (back > 0) set_sum(back, sum_at(back) - cnt);
    if (forw > 0) set_sum(forw, sum_at(forw) - cnt);
}

void ufs2_writer::mark_fragments_used(cylinder_group& cg, int start_frag, int count) {
    const int fpb = frags_per_block();
    const int end = start_frag + count;
    for (int f = start_frag; f < end;) {
        const int block_start = (f / fpb) * fpb;
        const int seg_end = end < block_start + fpb ? end : block_start + fpb;
        const auto old_runs = block_free_runs(cg, block_start, fpb);
        for (int g = f; g < seg_end; ++g)
            cg.raw_data[cg.free_blocks_offset + g / 8] &= static_cast<std::byte>(~(1 << (g % 8)));
        const auto new_runs = block_free_runs(cg, block_start, fpb);
        update_frsum(cg, old_runs, new_runs, fpb);

        int old_free = 0, new_free = 0;
        for (int r : old_runs) old_free += r;
        for (int r : new_runs) new_free += r;
        if (old_free == fpb && new_free != fpb) {
            put_i32(cg.raw_data, kNbfree, get_i32(cg.raw_data, kNbfree) - 1);
            cg.free_blocks = get_i32(cg.raw_data, kNbfree);
            put_i32(cg.raw_data, kNffree, get_i32(cg.raw_data, kNffree) + new_free);
        } else if (old_free != fpb) {
            put_i32(cg.raw_data, kNffree, get_i32(cg.raw_data, kNffree) - (old_free - new_free));
        }
        if (old_free == fpb && new_free != fpb) cluster_acct(cg, block_start / fpb, -1);
        f = seg_end;
    }
}

void ufs2_writer::mark_fragment_used(cylinder_group& cg, int frag_idx) {
    const int fpb = frags_per_block();
    const int block_start = (frag_idx / fpb) * fpb;
    const auto old_runs = block_free_runs(cg, block_start, fpb);

    cg.raw_data[cg.free_blocks_offset + frag_idx / 8] &= static_cast<std::byte>(~(1 << (frag_idx % 8)));

    const auto new_runs = block_free_runs(cg, block_start, fpb);
    update_frsum(cg, old_runs, new_runs, fpb);

    int old_free = 0, new_free = 0;
    for (int r : old_runs) old_free += r;
    for (int r : new_runs) new_free += r;

    if (old_free == fpb) {
        put_i32(cg.raw_data, kNbfree, get_i32(cg.raw_data, kNbfree) - 1);
        cg.free_blocks = get_i32(cg.raw_data, kNbfree);
        put_i32(cg.raw_data, kNffree, get_i32(cg.raw_data, kNffree) + new_free);
    } else {
        put_i32(cg.raw_data, kNffree, get_i32(cg.raw_data, kNffree) - 1);
    }

    if (old_free == fpb && new_free != fpb) cluster_acct(cg, frag_idx / fpb, -1);
}

namespace {
void put_u16(std::vector<std::byte>& d, std::size_t off, std::uint16_t v) {
    ps3hdd::write_be_u16(d.data() + off, v);
}
void put_be32(std::vector<std::byte>& d, std::size_t off, std::uint32_t v) {
    ps3hdd::write_be_u32(d.data() + off, v);
}
void put_be64(std::vector<std::byte>& d, std::size_t off, std::uint64_t v) {
    ps3hdd::write_be_u64(d.data() + off, v);
}
constexpr std::uint8_t DT_DIR = 4;
constexpr std::uint8_t DT_REG = 8;
} // namespace

std::uint64_t ufs2_writer::now() const {
    return clock_ ? clock_ : static_cast<std::uint64_t>(std::time(nullptr));
}

void ufs2_writer::patch_bytes(std::uint64_t offset, std::span<const std::byte> data) {
    constexpr std::uint32_t ss = 512;
    if (offset % ss == 0 && data.size() % ss == 0) {
        disk_.write_bytes(offset, data);
        return;
    }
    const std::uint64_t aligned_start = (offset / ss) * ss;
    const std::uint64_t aligned_end = ((offset + data.size() + ss - 1) / ss) * ss;
    auto buf = disk_.read_bytes(aligned_start, static_cast<std::size_t>(aligned_end - aligned_start));
    std::memcpy(buf.data() + (offset - aligned_start), data.data(), data.size());
    disk_.write_bytes(aligned_start, {buf.data(), buf.size()});
}

std::vector<std::byte> ufs2_writer::build_file_inode(std::int64_t file_size, std::span<const std::int64_t> direct_blocks, std::int64_t indirect, std::int64_t double_indirect, std::int64_t triple_indirect) {
    std::vector<std::byte> inode(256, std::byte{0});
    const std::uint64_t t = now();
    put_u16(inode, 0x00, 0x81FF); // S_IFREG | 0777
    put_u16(inode, 0x02, 1); // nlink
    put_be64(inode, 0x10, static_cast<std::uint64_t>(file_size));

    const bool has_indirect = indirect || double_indirect || triple_indirect;
    std::int64_t data_sectors;
    if (has_indirect) {
        const std::int64_t data_blocks = (file_size + sb_.block_size - 1) / sb_.block_size;
        data_sectors = data_blocks * (sb_.block_size / 512);
    } else {
        const std::int64_t frags = (file_size + sb_.fragment_size - 1) / sb_.fragment_size;
        data_sectors = frags * (sb_.fragment_size / 512);
    }
    std::int64_t indirect_blocks = (indirect ? 1 : 0) + (double_indirect ? 1 : 0);
    if (double_indirect) {
        const std::int64_t ppb = sb_.block_size / 8;
        const std::int64_t fs_blocks = (file_size + sb_.block_size - 1) / sb_.block_size;
        const std::int64_t in_double = fs_blocks - 12 - ppb;
        if (in_double > 0) indirect_blocks += (in_double + ppb - 1) / ppb;
    }
    put_be64(inode, 0x18, static_cast<std::uint64_t>(data_sectors + indirect_blocks * (sb_.block_size / 512)));
    put_be64(inode, 0x20, t);
    put_be64(inode, 0x28, t);
    put_be64(inode, 0x30, t);
    put_be64(inode, 0x38, t);
    put_be32(inode, 0x50, next_gen());
    for (std::size_t i = 0; i < 12 && i < direct_blocks.size(); ++i)
        put_be64(inode, 0x70 + i * 8, static_cast<std::uint64_t>(direct_blocks[i]));
    if (indirect) put_be64(inode, 0xD0, static_cast<std::uint64_t>(indirect));
    if (double_indirect) put_be64(inode, 0xD8, static_cast<std::uint64_t>(double_indirect));
    if (triple_indirect) put_be64(inode, 0xE0, static_cast<std::uint64_t>(triple_indirect));
    return inode;
}

std::vector<std::byte> ufs2_writer::build_directory_inode(std::int64_t data_block_frag, int nlink, std::uint16_t mode) {
    std::vector<std::byte> inode(256, std::byte{0});
    const std::uint64_t t = now();
    put_u16(inode, 0x00, mode);
    put_u16(inode, 0x02, static_cast<std::uint16_t>(nlink));
    put_be64(inode, 0x10, 512);                       // di_size (one DIRBLKSIZ)
    put_be64(inode, 0x18, sb_.block_size / 512);      // di_blocks (one full block)
    put_be64(inode, 0x20, t);
    put_be64(inode, 0x28, t);
    put_be64(inode, 0x30, t);
    put_be64(inode, 0x38, t);
    put_be32(inode, 0x50, next_gen());
    put_be64(inode, 0x70, static_cast<std::uint64_t>(data_block_frag));
    return inode;
}

std::vector<std::byte> ufs2_writer::build_empty_directory_block(std::uint64_t self_inode, std::uint64_t parent_inode) {
    std::vector<std::byte> block(static_cast<std::size_t>(sb_.block_size), std::byte{0});
    put_be32(block, 0x00, static_cast<std::uint32_t>(self_inode));
    put_u16(block, 0x04, 12);
    block[0x06] = static_cast<std::byte>(DT_DIR);
    block[0x07] = static_cast<std::byte>(1);
    block[0x08] = static_cast<std::byte>('.');
    put_be32(block, 0x0C, static_cast<std::uint32_t>(parent_inode));
    put_u16(block, 0x10, 512 - 12);
    block[0x12] = static_cast<std::byte>(DT_DIR);
    block[0x13] = static_cast<std::byte>(2);
    block[0x14] = static_cast<std::byte>('.');
    block[0x15] = static_cast<std::byte>('.');
    for (std::size_t sec = 512; sec + 6 <= block.size(); sec += 512)
        put_u16(block, sec + 4, 512);
    return block;
}

std::vector<std::byte> ufs2_writer::add_entry_to_directory_block(std::span<const std::byte> dir_block, std::uint64_t inode, const std::string& name, std::uint8_t dir_entry_type) {
    std::vector<std::byte> r(dir_block.begin(), dir_block.end());
    const int new_size = static_cast<int>(((8 + name.size() + 1 + 3) / 4) * 4);
    constexpr int DIRBLKSIZ = 512;
    const int sections = static_cast<int>(r.size()) / DIRBLKSIZ;

    for (int s = 0; s < sections; ++s) {
        const int start = s * DIRBLKSIZ, end = start + DIRBLKSIZ;
        int offset = start;
        while (offset < end) {
            const std::uint32_t ent_ino = ps3hdd::read_be_u32(r.data() + offset);
            const std::uint16_t rec_len = ps3hdd::read_be_u16(r.data() + offset + 4);
            if (rec_len == 0) break;
            const int actual = ent_ino == 0 ? 0
                : static_cast<int>(((8 + std::to_integer<int>(r[offset + 7]) + 1 + 3) / 4) * 4);
            if (rec_len - actual >= new_size) {
                if (actual > 0) put_u16(r, offset + 4, static_cast<std::uint16_t>(actual));
                const int no = offset + actual;
                const int nrl = rec_len - actual;
                std::fill(r.begin() + no, r.begin() + no + nrl, std::byte{0});
                put_be32(r, no, static_cast<std::uint32_t>(inode));
                put_u16(r, no + 4, static_cast<std::uint16_t>(nrl));
                r[no + 6] = static_cast<std::byte>(dir_entry_type);
                r[no + 7] = static_cast<std::byte>(name.size());
                std::memcpy(r.data() + no + 8, name.data(), name.size());
                return r;
            }
            offset += rec_len;
        }
    }
    throw std::runtime_error("directory block full: no room for entry");
}

void ufs2_writer::write_inode(std::uint64_t inode_number, std::span<const std::byte> inode_data) {
    const std::int64_t group = static_cast<std::int64_t>(inode_number) / sb_.inodes_per_group;
    const std::int64_t index = static_cast<std::int64_t>(inode_number) % sb_.inodes_per_group;
    const std::uint64_t cg_offset = fs_.partition_offset_bytes() +
        static_cast<std::uint64_t>(group) * sb_.frags_per_group * sb_.fragment_size;
    const std::uint64_t table = cg_offset + sb_.inode_block_offset * sb_.fragment_size;
    patch_bytes(table + static_cast<std::uint64_t>(index) * superblock::inode_size, inode_data);
}

void ufs2_writer::write_data_block(std::int64_t frag, std::span<const std::byte> data) {
    patch_bytes(fs_.partition_offset_bytes() + static_cast<std::uint64_t>(frag) * sb_.fragment_size, data);
}

bool ufs2_writer::directory_contains_entry(std::uint64_t parent_inode, const std::string& name) {
    const auto dir = fs_.read_inode(parent_inode);
    if (!dir.is_directory()) return false;
    for (const auto& e : fs_.read_directory(dir))
        if (e.name == name) return true;
    return false;
}

void ufs2_writer::add_entry_to_directory(std::uint64_t parent_inode, std::uint64_t child_inode, const std::string& name, std::uint8_t dir_entry_type) {
    const std::int64_t pg = static_cast<std::int64_t>(parent_inode) / sb_.inodes_per_group;
    const std::int64_t pidx = static_cast<std::int64_t>(parent_inode) % sb_.inodes_per_group;
    const std::uint64_t p_off = fs_.partition_offset_bytes() +
        static_cast<std::uint64_t>(pg) * sb_.frags_per_group * sb_.fragment_size +
        sb_.inode_block_offset * sb_.fragment_size + static_cast<std::uint64_t>(pidx) * superblock::inode_size;
    auto pinode = disk_.read_bytes(p_off, superblock::inode_size);

    const std::int64_t block_bytes = sb_.block_size; // directory blocks are whole blocks
    auto save_inode = [&]() { write_inode(parent_inode, pinode); };

    for (int i = 0; i < 12; ++i) {
        const std::int64_t frag = static_cast<std::int64_t>(ps3hdd::read_be_u64(pinode.data() + 0x70 + i * 8));
        if (frag == 0) {
            // all existing blocks are full: allocate a new directory fragment
            const int parent_cg = static_cast<int>(parent_inode / sb_.inodes_per_group);
            cylinder_group* block_cg = &read_cylinder_group(parent_cg);
            const std::int64_t abs = alloc_run(frags_per_block(), block_cg); // may span cgs!

            std::vector<std::byte> block(static_cast<std::size_t>(block_bytes), std::byte{0});
            for (std::size_t sec = 0; sec + 6 <= block.size(); sec += 512)
                put_u16(block, sec + 4, 512); // empty record per DIRBLKSIZ section
            auto updated = add_entry_to_directory_block(block, child_inode, name, dir_entry_type);
            write_data_block(abs, updated);

            put_be64(pinode, 0x70 + i * 8, static_cast<std::uint64_t>(abs));
            // di_size covers only the used DIRBLKSIZ sections of this new block, not the whole fs block which would claim unallocated frags
            put_be64(pinode, 0x10, static_cast<std::uint64_t>(static_cast<std::int64_t>(i) * block_bytes + dir_block_used_bytes(updated))); // di_size
            put_be64(pinode, 0x18, ps3hdd::read_be_u64(pinode.data() + 0x18) + block_bytes / 512); // di_blocks
            save_inode();
            return;
        }

        const std::int64_t cur_size = static_cast<std::int64_t>(ps3hdd::read_be_u64(pinode.data() + 0x10));
        const std::int64_t alloc_bytes =
            static_cast<std::int64_t>(ps3hdd::read_be_u64(pinode.data() + 0x18)) * 512;
        const std::int64_t block_start = static_cast<std::int64_t>(i) * block_bytes;
        std::int64_t extent = alloc_bytes - block_start; // allocated bytes in this block
        if (extent > block_bytes) extent = block_bytes;
        if (extent < sb_.fragment_size) extent = sb_.fragment_size; // this block holds >= 1 fragment

        const std::uint64_t off = fs_.partition_offset_bytes() +
            static_cast<std::uint64_t>(frag) * sb_.fragment_size;
        auto block = disk_.read_bytes(off, static_cast<std::size_t>(extent));
        try {
            auto updated = add_entry_to_directory_block(block, child_inode, name, dir_entry_type);
            write_data_block(frag, updated);
            const std::int64_t need = block_start + dir_block_used_bytes(updated);
            if (cur_size < need) {
                put_be64(pinode, 0x10, static_cast<std::uint64_t>(need));
                save_inode();
            }
            return;
        } catch (const std::runtime_error&) {
            if (extent < block_bytes) {
                const int ofrags = static_cast<int>(extent / sb_.fragment_size);
                const int nfrags = std::min(ofrags + 1, frags_per_block());
                if (!frag_extend(frag, ofrags, nfrags))
                    throw std::runtime_error("directory fragment full and cannot be extended in place");

                const std::int64_t new_extent = static_cast<std::int64_t>(nfrags) * sb_.fragment_size;
                std::vector<std::byte> grown(static_cast<std::size_t>(new_extent), std::byte{0});
                std::memcpy(grown.data(), block.data(), block.size());
                for (std::int64_t sec = extent; sec + 6 <= new_extent; sec += superblock::dir_block_size)
                    put_u16(grown, static_cast<std::size_t>(sec) + 4, superblock::dir_block_size);

                auto updated = add_entry_to_directory_block(grown, child_inode, name, dir_entry_type);
                write_data_block(frag, updated);
                put_be64(pinode, 0x10, static_cast<std::uint64_t>(block_start + dir_block_used_bytes(updated)));
                put_be64(pinode, 0x18,
                         ps3hdd::read_be_u64(pinode.data() + 0x18) +
                             static_cast<std::uint64_t>(nfrags - ofrags) * sb_.fragment_size / 512);
                save_inode();
                return;
            }
        }
    }
    throw std::runtime_error("directory too large: needs indirect directory blocks (not yet supported)");
}

bool ufs2_writer::frag_extend(std::int64_t abs_frag, int ofrags, int nfrags) {
    const int fpb = frags_per_block();
    const int fpg = static_cast<int>(sb_.frags_per_group);
    if (nfrags <= ofrags || nfrags > fpb || abs_frag <= 0 || fpg <= 0) return false;

    const int cgn = static_cast<int>(abs_frag / fpg);
    const int bno = static_cast<int>(abs_frag % fpg);
    if (cgn < 0 || cgn >= sb_.cylinder_groups) return false;
    if ((bno % fpb) + nfrags > fpb) return false;

    auto& cg = read_cylinder_group(cgn);
    if (cg.magic != cylinder_group::magic_value) return false;
    for (int i = ofrags; i < nfrags; ++i) {
        const int f = bno + i;
        if (f < 0 || f >= fpg) return false;
        if (!bit_set(cg.raw_data, static_cast<std::size_t>(cg.free_blocks_offset) + f / 8, f % 8)) return false;
        if (protected_fragments_.count(static_cast<std::int64_t>(cgn) * fpg + f)) return false;
    }
    mark_fragments_used(cg, bno + ofrags, nfrags - ofrags);
    write_cylinder_group(cg);
    return true;
}

std::int64_t ufs2_writer::alloc_run(int count, cylinder_group*& block_cg) {
    std::int64_t frag = find_free_fragments(*block_cg, count);
    if (frag < 0) {
        const int ncg = sb_.cylinder_groups;
        for (int i = 1; i <= ncg; ++i) {
            auto& cg = read_cylinder_group((block_cg->number + i) % ncg);
            if (cg.magic != cylinder_group::magic_value) continue;
            frag = find_free_fragments(cg, count);
            if (frag >= 0) { block_cg = &cg; break; }
        }
        if (frag < 0) throw std::runtime_error("disk full: no free space in any cylinder group!");
    }
    mark_fragments_used(*block_cg, static_cast<int>(frag), count);
    write_cylinder_group(*block_cg);
    return static_cast<std::int64_t>(block_cg->number) * sb_.frags_per_group + frag;
}

bool ufs2_writer::fragments_free_at(const cylinder_group& cg, int start, int count) const {
    const int fpg = static_cast<int>(sb_.frags_per_group);
    const int fpb = frags_per_block();
    if (start < static_cast<int>(sb_.data_block_offset) || start + count > fpg) return false;

    if (count >= fpb) {
        if (start % fpb != 0) return false;
    } else if ((start % fpb) + count > fpb) {
        return false;
    }
    for (int i = 0; i < count; ++i) {
        const int f = start + i;
        if (!bit_set(cg.raw_data, static_cast<std::size_t>(cg.free_blocks_offset) + f / 8, f % 8)) return false;
        const std::int64_t global = static_cast<std::int64_t>(cg.number) * sb_.frags_per_group + f;
        if (protected_fragments_.count(global)) return false;
    }
    return true;
}

std::int64_t ufs2_writer::alloc_run_pref(int count, std::int64_t pref, cylinder_group*& block_cg) {
    const int fpg = static_cast<int>(sb_.frags_per_group);
    const int ncg = sb_.cylinder_groups;
    if (fpg <= 0 || ncg <= 0) return alloc_run(count, block_cg);

    const int pref_cg = pref > 0 ? static_cast<int>((pref / fpg) % ncg) : (block_cg ? block_cg->number : 0);

    cylinder_group* found = nullptr;
    std::int64_t found_frag = -1;
    const std::int64_t got = ffs_hashalloc(policy_, pref_cg, pref, [&](int cg, std::int64_t p) -> std::int64_t {
        auto& c = read_cylinder_group(cg);
        if (c.magic != cylinder_group::magic_value) return 0;
        if (p > 0 && (p / fpg) == cg) {
            const int local = static_cast<int>(p % fpg);
            if (fragments_free_at(c, local, count)) {
                found = &c;
                found_frag = local;
                return static_cast<std::int64_t>(cg) * fpg + local;
            }
        }
        const int hint = (p > 0 && (p / fpg) == cg) ? static_cast<int>(p % fpg) : -1;
        const std::int64_t f = find_free_fragments(c, count, hint);
        if (f < 0) return 0;
        found = &c;
        found_frag = f;
        return static_cast<std::int64_t>(cg) * fpg + f;
    });

    if (got <= 0 || !found) throw std::runtime_error("disk full: no free space in any cylinder group!");
    block_cg = found;
    mark_fragments_used(*found, static_cast<int>(found_frag), count);
    write_cylinder_group(*found);
    return got;
}

int ufs2_writer::repair_used_but_free(const std::vector<std::int64_t>& frags) {
    const int fpg = static_cast<int>(sb_.frags_per_group);
    if (fpg <= 0) return 0;
    int fixed = 0;
    for (const std::int64_t abs : frags) {
        if (abs < 0) continue;
        const int cgn = static_cast<int>(abs / fpg);
        const int idx = static_cast<int>(abs % fpg);
        if (cgn < 0 || cgn >= sb_.cylinder_groups) continue;
        auto& cg = read_cylinder_group(cgn);
        if (cg.magic != cylinder_group::magic_value) continue;
        if (!bit_set(cg.raw_data, static_cast<std::size_t>(cg.free_blocks_offset) + idx / 8, idx % 8))
            continue;
        mark_fragment_used(cg, idx);
        write_cylinder_group(cg);
        ++fixed;
    }
    return fixed;
}

int ufs2_writer::reclaim_orphan_inodes(const std::vector<std::int64_t>& inodes,const std::vector<bool>& claimed) {
    const std::int64_t ipg = sb_.inodes_per_group;
    if (ipg <= 0) return 0;
    const int fpb = frags_per_block();
    int done = 0;
    auto owned_by_live_file = [&](std::int64_t frag, int count) {
        for (int k = 0; k < count; ++k) {
            const std::int64_t f = frag + k;
            if (f >= 0 && static_cast<std::size_t>(f) < claimed.size() && claimed[static_cast<std::size_t>(f)])
                return true;
        }
        return false;
    };
    for (const std::int64_t ino : inodes) {
        if (ino < 2) continue; // 0 and 1 are reserved
        inode in;
        try {
            in = fs_.read_inode(static_cast<std::uint64_t>(ino));
        } catch (const std::exception&) {
            continue;
        }
        const int cgn = static_cast<int>(ino / ipg);
        if (cgn < 0 || cgn >= sb_.cylinder_groups) continue;
        auto& cg = read_cylinder_group(cgn);
        if (cg.magic != cylinder_group::magic_value) continue;
        const int idx = static_cast<int>(ino % ipg);
        //now skip anything already free ... refreeing would corrupt the counters
        if (!bit_set(cg.raw_data, static_cast<std::size_t>(cg.inodes_used_offset) + idx / 8, idx % 8))
            continue;

        for (const auto ptr : fs_.block_pointers(in)) {
            if (ptr <= 0) continue;
            if (owned_by_live_file(ptr, fpb)) continue;
            free_block_run(ptr, fpb);
        }
        mark_inode_free(cg, idx);
        if (in.is_directory())
            put_i32(cg.raw_data, 0x18, get_i32(cg.raw_data, 0x18) - 1);
        write_cylinder_group(cg);

        // blank dinode so nothing can mistake it for a live file later]
        std::vector<std::byte> zero(superblock::inode_size, std::byte{0});
        write_inode(static_cast<std::uint64_t>(ino), zero);
        ++done;
    }
    return done;
}

std::uint64_t ufs2_writer::allocate_inode(int start_cg, cylinder_group*& out_cg, int& out_idx) {
    const int ncg = sb_.cylinder_groups;
    for (int i = 0; i < ncg; ++i) {
        const int cgi = (start_cg + i) % ncg;
        auto& cg = read_cylinder_group(cgi);
        const int idx = use_lv2_policy_ ? find_free_inode_rotor(cg) : find_free_inode(cg);
        if (idx >= 0) {
            out_cg = &cg;
            out_idx = idx;
            return static_cast<std::uint64_t>(cgi) * sb_.inodes_per_group + idx;
        }
    }
    throw std::runtime_error("no free inodes on disk");
}

std::uint64_t ufs2_writer::create_directory(std::uint64_t parent_inode, const std::string& name) {
    if (directory_contains_entry(parent_inode, name))
        throw std::runtime_error("directory already contains the entry");

    const int parent_cg = static_cast<int>(parent_inode / sb_.inodes_per_group);
    cylinder_group* icg = nullptr;
    int idx = 0;
    std::uint64_t new_inode = 0;

    if (use_lv2_policy_) {
        // lv2 here picks the cylinder group for a new dir with ffs_dirpref, then hands that preference to ffs_hashalloc, which falls back through a quadratic rehash
        // and finally a full scan. reproducing both is what spreads a games directory tree across groups the way a console written disk looks
        
        // !!!DELIBERATE DEVIATION!!! 
        // the kernel seeds the first level dir branch with arc4random()
        
        //now use the writers existing deterministic PRNG instead, so an install is reproducible run to run. 
        // placement is random on the console by design! so any draw is equally faithful but a fixed one is far easier to diff and to debug 
        
        const bool root_child = (parent_inode == ufs2_filesystem::root_inode);
        const std::int64_t pref_ino = ffs_dirpref(policy_, parent_inode, root_child, next_gen());
        const int pref_cg = sb_.inodes_per_group > 0 ? static_cast<int>(pref_ino / sb_.inodes_per_group) % std::max(1, sb_.cylinder_groups) : parent_cg;
        cylinder_group* found = nullptr;
        int found_idx = -1;
        const std::int64_t got = ffs_hashalloc(policy_, pref_cg, pref_ino, [&](int cg, std::int64_t) -> std::int64_t {
            auto& c = read_cylinder_group(cg);
            if (c.magic != cylinder_group::magic_value) return 0;
            const int i = find_free_inode_rotor(c);
            if (i < 0) return 0;
            found = &c;
            found_idx = i;
            // 0 means "nothing here" to hashalloc
            // inode 0 is reserved so this is safe!
            return static_cast<std::int64_t>(cg) * sb_.inodes_per_group + i;
        });
        if (got <= 0 || !found) throw std::runtime_error("no free inodes on disk");
        icg = found;
        idx = found_idx;
        new_inode = static_cast<std::uint64_t>(got);
    } else {
        new_inode = allocate_inode(parent_cg, icg, idx);
    }

    cylinder_group* block_cg = icg;
    const std::int64_t abs_frag = alloc_run(frags_per_block(), block_cg);

    auto dir_block = build_empty_directory_block(new_inode, parent_inode);
    write_data_block(abs_frag, dir_block);

    mark_inode_used(*icg, idx); // zero inode block (if extending) before writing
    auto inode_bytes = build_directory_inode(abs_frag, /*nlink=*/2);
    write_inode(new_inode, inode_bytes);

    put_i32(icg->raw_data, 0x18, get_i32(icg->raw_data, 0x18) + 1);
    write_cylinder_group(*icg);

    if (use_lv2_policy_) {
        const std::size_t c = static_cast<std::size_t>(icg->number);
        if (c < policy_.contig_dirs.size() && policy_.contig_dirs[c] < 255) ++policy_.contig_dirs[c];
    }

    add_entry_to_directory(parent_inode, new_inode, name, DT_DIR);

    const std::int64_t pg = static_cast<std::int64_t>(parent_inode) / sb_.inodes_per_group;
    const std::int64_t pidx = static_cast<std::int64_t>(parent_inode) % sb_.inodes_per_group;
    const std::uint64_t p_off = fs_.partition_offset_bytes() +
        static_cast<std::uint64_t>(pg) * sb_.frags_per_group * sb_.fragment_size +
        sb_.inode_block_offset * sb_.fragment_size +
        static_cast<std::uint64_t>(pidx) * superblock::inode_size;
    auto parent_raw = disk_.read_bytes(p_off, superblock::inode_size);
    put_u16(parent_raw, 0x02, static_cast<std::uint16_t>(ps3hdd::read_be_u16(parent_raw.data() + 0x02) + 1));
    write_inode(parent_inode, parent_raw);
    return new_inode;
}

std::uint64_t ufs2_writer::write_file(std::uint64_t parent_inode, const std::string& name, std::span<const std::byte> data) {
    std::size_t off = 0;
    return write_file(parent_inode, name, static_cast<std::int64_t>(data.size()), [&](std::span<std::byte> dst) {
        std::memcpy(dst.data(), data.data() + off, dst.size());
        off += dst.size();
    });
}

std::uint64_t ufs2_writer::write_file(std::uint64_t parent_inode, const std::string& name, std::int64_t size, const std::function<void(std::span<std::byte>)>& fill, const std::function<void(std::int64_t)>& on_written) {
    if (directory_contains_entry(parent_inode, name))
        throw std::runtime_error("directory already contains the entry");

    const int parent_cg = static_cast<int>(parent_inode / sb_.inodes_per_group);
    cylinder_group* icg = nullptr;
    int idx = 0;
    const std::uint64_t new_inode = allocate_inode(parent_cg, icg, idx);

    const int fpb = frags_per_block();
    const std::int64_t ppb = sb_.block_size / 8;
    const std::int64_t blocks_needed = (size + sb_.block_size - 1) / sb_.block_size;
    if (blocks_needed > 12 + ppb + ppb * ppb)
        throw std::runtime_error("write_file: triple-indirect files (> ~68 GB) are not supported");

    const bool needs_indirect = blocks_needed > 12;
    
    cylinder_group* block_cg = icg;

    std::vector<std::int64_t> data_blocks;
    data_blocks.reserve(static_cast<std::size_t>(blocks_needed));
    std::int64_t remaining = size;

    constexpr std::size_t kWriteBatch = 8u * 1024 * 1024;
    std::vector<std::byte> batch;
    batch.reserve(kWriteBatch + static_cast<std::size_t>(sb_.block_size));
    std::int64_t batch_frag = -1; // first frag of the pending batch
    std::int64_t batch_end = -1;  // frag the batch expects next (for contiguity...)
    auto flush_batch = [&]() {
        if (batch.empty()) return;
        write_data_block(batch_frag, batch);
        if (on_written) on_written(static_cast<std::int64_t>(batch.size())); // report @ real disk write
        batch.clear();
        batch_frag = -1;
    };

    for (std::int64_t b = 0; b < blocks_needed; ++b) {
        const bool is_last = b == blocks_needed - 1;
        int frags_this = fpb;
        if (is_last && !needs_indirect) {
            frags_this = static_cast<int>((remaining + sb_.fragment_size - 1) / sb_.fragment_size);
            frags_this = std::max(1, std::min(frags_this, fpb));
        }
        std::int64_t abs_frag;
        if (use_lv2_policy_) {
            // lv2 asks ffs_blkpref_ufs2 where this block actually wants to go before allocating
            // indx is the slot within whichever pointer array holds this block: the inode's own 12 direct slots, or a slot inside an indirect block. 
            // bap[indx-1]
            // is simply the block allocated just before it in that same array, which is what wmakes the common case a contiguous append
            const int indx = (b < superblock::direct_blocks) ? static_cast<int>(b) : static_cast<int>((b - superblock::direct_blocks) % ppb);
            const std::int64_t prev = (indx > 0 && b > 0) ? data_blocks[static_cast<std::size_t>(b - 1)] : 0;
            const std::int64_t pref = ffs_blkpref_ufs2_prev(policy_, new_inode, b, indx, prev);
            abs_frag = alloc_run_pref(frags_this, pref, block_cg);
        } else {
            abs_frag = alloc_run(frags_this, block_cg);
        }

        const std::size_t wb = static_cast<std::size_t>(frags_this) * sb_.fragment_size;
        if (batch_frag >= 0 && (abs_frag != batch_end || batch.size() + wb > kWriteBatch))
            flush_batch();
        if (batch_frag < 0) batch_frag = abs_frag;

        const std::size_t base = batch.size();
        batch.resize(base + wb, std::byte{0});
        const std::size_t to_copy = static_cast<std::size_t>(std::min<std::int64_t>(wb, remaining));
        fill({batch.data() + base, to_copy});
        batch_end = abs_frag + frags_this;

        data_blocks.push_back(abs_frag);
        remaining -= to_copy;
    }
    flush_batch();

    std::array<std::int64_t, 12> direct{};
    for (std::int64_t i = 0; i < 12 && i < blocks_needed; ++i)
        direct[static_cast<std::size_t>(i)] = data_blocks[static_cast<std::size_t>(i)];

    std::int64_t indirect = 0, double_indirect = 0;

    // an indirect (pointer) block is metadata, not file data! 
    // The kernel asks blkpref for it too, but passes a null bap.
    
    // so there is no predecessor and it falls through to the "own cylinder group / rotor" cases rather than appending..
    //
    auto alloc_meta_block = [&](std::int64_t lbn) -> std::int64_t {
        if (!use_lv2_policy_) return alloc_run(fpb, block_cg);
        const std::int64_t pref = ffs_blkpref_ufs2_prev(policy_, new_inode, lbn, 0, 0);
        return alloc_run_pref(fpb, pref, block_cg);
    };

    auto write_pointer_block = [&](std::int64_t base, std::int64_t count) -> std::int64_t {
        const std::int64_t frag = alloc_meta_block(base);
        std::vector<std::byte> blk(static_cast<std::size_t>(sb_.block_size), std::byte{0});
        for (std::int64_t j = 0; j < count; ++j)
            put_be64(blk, static_cast<std::size_t>(j) * 8, static_cast<std::uint64_t>(data_blocks[static_cast<std::size_t>(base + j)]));
        write_data_block(frag, blk);
        return frag;
    };

    if (blocks_needed > 12) {
        const std::int64_t count = std::min(blocks_needed - 12, ppb);
        indirect = write_pointer_block(12, count);
    }

    if (blocks_needed > 12 + ppb) {
        std::vector<std::byte> dbl(static_cast<std::size_t>(sb_.block_size), std::byte{0});
        const std::int64_t l1_count = (blocks_needed - 12 - ppb + ppb - 1) / ppb;
        for (std::int64_t l1 = 0; l1 < l1_count; ++l1) {
            const std::int64_t base = 12 + ppb + l1 * ppb;
            const std::int64_t count = std::min(ppb, blocks_needed - base);
            const std::int64_t l1_frag = write_pointer_block(base, count);
            put_be64(dbl, static_cast<std::size_t>(l1) * 8, static_cast<std::uint64_t>(l1_frag));
        }
        double_indirect = alloc_meta_block(12 + ppb);
        write_data_block(double_indirect, dbl);
    }

    mark_inode_used(*icg, idx);
    auto inode_bytes = build_file_inode(size, direct, indirect, double_indirect);
    write_inode(new_inode, inode_bytes);
    write_cylinder_group(*icg);

    add_entry_to_directory(parent_inode, new_inode, name, DT_REG);
    return new_inode;
}

void ufs2_writer::update_superblock() {
    const std::vector<int> changed(dirty_cgs_.begin(), dirty_cgs_.end());
    flush_dirty_cgs();
    if (changed.empty()) return;

    const std::uint64_t sb_off = fs_.partition_offset_bytes() + 65536;
    auto sbdata = disk_.read_bytes(sb_off, 8192);

    std::int64_t ndir      = static_cast<std::int64_t>(ps3hdd::read_be_u64(sbdata.data() + 0x3F0));
    std::int64_t nbfree    = static_cast<std::int64_t>(ps3hdd::read_be_u64(sbdata.data() + 0x3F8));
    std::int64_t nifree    = static_cast<std::int64_t>(ps3hdd::read_be_u64(sbdata.data() + 0x400));
    std::int64_t nffree    = static_cast<std::int64_t>(ps3hdd::read_be_u64(sbdata.data() + 0x408));
    std::int64_t nclusters = static_cast<std::int64_t>(ps3hdd::read_be_u64(sbdata.data() + 0x410));

    const std::int64_t cs_addr = static_cast<std::int64_t>(ps3hdd::read_be_u64(sbdata.data() + 0x448));
    const std::int32_t cs_size = get_i32(sbdata, 0x9C);
    std::vector<std::byte> cs_data;
    std::uint64_t cs_off = 0;
    if (cs_addr > 0 && cs_size > 0) {
        cs_off = fs_.partition_offset_bytes() + static_cast<std::uint64_t>(cs_addr) * sb_.fragment_size;
        cs_data = disk_.read_bytes(cs_off, static_cast<std::size_t>(cs_size));
    }

    for (int i : changed) {
        auto& cg = read_cylinder_group(i);
        const auto cur = compute_cg_summary(cg);
        auto base_it = cg_summary_base_.find(i);
        const std::array<std::int64_t, 5> base =
            base_it != cg_summary_base_.end() ? base_it->second : cur;
        ndir      += cur[0] - base[0];
        nbfree    += cur[1] - base[1];
        nifree    += cur[2] - base[2];
        nffree    += cur[3] - base[3];
        nclusters += cur[4] - base[4];
        cg_summary_base_[i] = cur; // new baseline for the next update

        const std::size_t cs_ent = static_cast<std::size_t>(i) * 16;
        if (!cs_data.empty() && cs_ent + 16 <= cs_data.size()) {
            put_i32(cs_data, cs_ent + 0, static_cast<std::int32_t>(cur[0]));
            put_i32(cs_data, cs_ent + 4, static_cast<std::int32_t>(cur[1]));
            put_i32(cs_data, cs_ent + 8, static_cast<std::int32_t>(cur[2]));
            put_i32(cs_data, cs_ent + 12, static_cast<std::int32_t>(cur[3]));
        }
    }

    ps3hdd::write_be_u64(sbdata.data() + 0x3F0, static_cast<std::uint64_t>(ndir));
    ps3hdd::write_be_u64(sbdata.data() + 0x3F8, static_cast<std::uint64_t>(nbfree));
    ps3hdd::write_be_u64(sbdata.data() + 0x400, static_cast<std::uint64_t>(nifree));
    ps3hdd::write_be_u64(sbdata.data() + 0x408, static_cast<std::uint64_t>(nffree));
    ps3hdd::write_be_u64(sbdata.data() + 0x410, static_cast<std::uint64_t>(nclusters));
    disk_.write_bytes(sb_off, {sbdata.data(), sbdata.size()});

    if (cs_off != 0 && !cs_data.empty())
        patch_bytes(cs_off, {cs_data.data(), cs_data.size()}); // rmw for sector alignment
    disk_.flush();
}

int ufs2_writer::repair_free_counts(const std::function<void(int, int)>& progress) {
    const int fpb = frags_per_block();
    const int fpg = static_cast<int>(sb_.frags_per_group);
    const int ipg = static_cast<int>(sb_.inodes_per_group);
    const int ncg = sb_.cylinder_groups;
    int fixed = 0;
    for (int cgn = 0; cgn < ncg; ++cgn) {
        auto& cg = read_cylinder_group(cgn);
        if (cg.magic == cylinder_group::magic_value) {
            const int freeoff = cg.free_blocks_offset;
            const int iusedoff = cg.inodes_used_offset;
            auto bit = [&](int base, int idx) {
                const std::size_t b = static_cast<std::size_t>(base) + idx / 8;
                return b < cg.raw_data.size() && ((std::to_integer<int>(cg.raw_data[b]) >> (idx % 8)) & 1);
            };
            std::int64_t nbfree = 0, nffree = 0;
            std::vector<std::int64_t> frsum(fpb, 0);
            for (int base = 0; base + fpb <= fpg; base += fpb) {
                int freec = 0, run = 0;
                std::vector<int> runs;
                for (int f = 0; f < fpb; ++f) {
                    if (bit(freeoff, base + f)) { ++freec; ++run; }
                    else if (run > 0) { runs.push_back(run); run = 0; }
                }
                if (run > 0) runs.push_back(run);
                if (freec == fpb) ++nbfree;
                else { nffree += freec; for (int r : runs) if (r >= 1 && r < fpb) ++frsum[r]; }
            }
            std::int64_t nifree = 0;
            for (int i = 0; i < ipg; ++i) if (!bit(iusedoff, i)) ++nifree;

            bool changed = false;
            if (get_i32(cg.raw_data, 0x1C) != nbfree) { put_i32(cg.raw_data, 0x1C, static_cast<std::int32_t>(nbfree)); changed = true; }
            if (get_i32(cg.raw_data, 0x20) != nifree) { put_i32(cg.raw_data, 0x20, static_cast<std::int32_t>(nifree)); changed = true; }
            if (get_i32(cg.raw_data, 0x24) != nffree) { put_i32(cg.raw_data, 0x24, static_cast<std::int32_t>(nffree)); changed = true; }
            for (int i = 1; i < fpb; ++i)
                if (get_i32(cg.raw_data, 0x34 + i * 4) != frsum[i]) {
                    put_i32(cg.raw_data, 0x34 + i * 4, static_cast<std::int32_t>(frsum[i]));
                    changed = true;
                }

            const int contigsum = sb_.contig_sum_size;
            const int clusteroff = get_i32(cg.raw_data, kClusteroff);
            const int clustersumoff = get_i32(cg.raw_data, kClustersumoff);
            const int nclusterblks = get_i32(cg.raw_data, kNclusterblks);
            if (contigsum > 0 && clusteroff > 0 && clustersumoff > 0 && nclusterblks > 0) {
                std::vector<std::uint8_t> want(static_cast<std::size_t>(nclusterblks), 0);
                for (int b = 0; b < nclusterblks; ++b) {
                    bool all_free = true;
                    for (int k = 0; k < fpb && all_free; ++k)
                        if (!bit(freeoff, b * fpb + k)) all_free = false;
                    want[static_cast<std::size_t>(b)] = all_free ? 1 : 0;
                }
                for (int b = 0; b < nclusterblks; ++b) {
                    const std::size_t byte = static_cast<std::size_t>(clusteroff) + b / 8;
                    if (byte >= cg.raw_data.size()) break;
                    const bool now = (std::to_integer<int>(cg.raw_data[byte]) >> (b % 8)) & 1;
                    if (now == (want[static_cast<std::size_t>(b)] != 0)) continue;
                    if (want[static_cast<std::size_t>(b)])
                        cg.raw_data[byte] |= static_cast<std::byte>(1 << (b % 8));
                    else
                        cg.raw_data[byte] &= static_cast<std::byte>(~(1 << (b % 8)));
                    changed = true;
                }
                std::vector<std::int32_t> csum(static_cast<std::size_t>(contigsum) + 1, 0);
                int run = 0;
                for (int b = 0; b <= nclusterblks; ++b) {
                    const bool free_here = b < nclusterblks && want[static_cast<std::size_t>(b)];
                    if (free_here) { ++run; continue; }
                    if (run > 0) ++csum[static_cast<std::size_t>(std::min(run, contigsum))];
                    run = 0;
                }
                for (int i = 1; i <= contigsum; ++i)
                    if (get_i32(cg.raw_data, static_cast<std::size_t>(clustersumoff) + 4 * i) != csum[static_cast<std::size_t>(i)]) {
                        put_i32(cg.raw_data, static_cast<std::size_t>(clustersumoff) + 4 * i, csum[static_cast<std::size_t>(i)]);
                        changed = true;
                    }
            }
            if (changed) {
                cg.free_blocks = get_i32(cg.raw_data, 0x1C);
                cg.free_inodes = get_i32(cg.raw_data, 0x20);
                write_cylinder_group(cg);
                ++fixed;
            }
        }
        if (progress && (cgn % 64 == 0 || cgn == ncg - 1)) progress(cgn + 1, ncg);
    }
    update_superblock();
    return fixed;
}

void ufs2_writer::mark_fragment_free(cylinder_group& cg, int frag_idx) {
    const int fpb = frags_per_block();
    const int block_start = (frag_idx / fpb) * fpb;
    const auto old_runs = block_free_runs(cg, block_start, fpb);

    cg.raw_data[cg.free_blocks_offset + frag_idx / 8] |= static_cast<std::byte>(1 << (frag_idx % 8));

    const auto new_runs = block_free_runs(cg, block_start, fpb);
    update_frsum(cg, old_runs, new_runs, fpb);
    int old_free = 0, new_free = 0;
    for (int r : old_runs) old_free += r;
    for (int r : new_runs) new_free += r;
    if (new_free == fpb) {
        put_i32(cg.raw_data, 0x24, get_i32(cg.raw_data, 0x24) - old_free);
        put_i32(cg.raw_data, 0x1C, get_i32(cg.raw_data, 0x1C) + 1);
        cg.free_blocks = get_i32(cg.raw_data, 0x1C);
    } else {
        put_i32(cg.raw_data, 0x24, get_i32(cg.raw_data, 0x24) + 1);
    }
    if (old_free != fpb && new_free == fpb) cluster_acct(cg, frag_idx / fpb, +1);
}

void ufs2_writer::mark_fragments_free(cylinder_group& cg, int start_frag, int count) {
    const int fpb = frags_per_block();
    const int end = start_frag + count;
    for (int f = start_frag; f < end;) {
        const int block_start = (f / fpb) * fpb;
        const int seg_end = end < block_start + fpb ? end : block_start + fpb;
        const auto old_runs = block_free_runs(cg, block_start, fpb);
        for (int g = f; g < seg_end; ++g)
            cg.raw_data[cg.free_blocks_offset + g / 8] |= static_cast<std::byte>(1 << (g % 8));
        const auto new_runs = block_free_runs(cg, block_start, fpb);
        update_frsum(cg, old_runs, new_runs, fpb);

        int old_free = 0, new_free = 0;
        for (int r : old_runs) old_free += r;
        for (int r : new_runs) new_free += r;
        if (old_free != fpb && new_free == fpb) {
            put_i32(cg.raw_data, 0x24, get_i32(cg.raw_data, 0x24) - old_free);
            put_i32(cg.raw_data, 0x1C, get_i32(cg.raw_data, 0x1C) + 1);
            cg.free_blocks = get_i32(cg.raw_data, 0x1C);
        } else if (new_free != fpb) {
            put_i32(cg.raw_data, 0x24, get_i32(cg.raw_data, 0x24) + (new_free - old_free));
        }
        if (old_free != fpb && new_free == fpb) cluster_acct(cg, block_start / fpb, +1);
        f = seg_end;
    }
}

void ufs2_writer::mark_inode_free(cylinder_group& cg, int inode_idx) {
    cg.raw_data[cg.inodes_used_offset + inode_idx / 8] &= static_cast<std::byte>(~(1 << (inode_idx % 8)));
    cg.free_inodes++;
    put_i32(cg.raw_data, 0x20, cg.free_inodes);
}

void ufs2_writer::free_block_run(std::int64_t abs_frag, int count) {
    const int cg_num = static_cast<int>(abs_frag / sb_.frags_per_group);
    const int frag = static_cast<int>(abs_frag % sb_.frags_per_group);
    auto& cg = read_cylinder_group(cg_num);
    mark_fragments_free(cg, frag, count);
    write_cylinder_group(cg);
}

void ufs2_writer::free_inode_blocks(const inode& in) {
    const int fpb = frags_per_block();
    const std::int64_t ppb = sb_.block_size / 8;
    const std::int64_t nblocks = (in.size + sb_.block_size - 1) / sb_.block_size;
    const bool needs_indirect = nblocks > 12;

    //hardening
    auto frag_ok = [&](std::int64_t f) {
        return f > 0 && (sb_.total_fragments <= 0 || f + fpb <= sb_.total_fragments);
    };
    auto free_ptr = [&](std::int64_t f) { if (frag_ok(f)) free_block_run(f, fpb); };
    auto try_read = [&](std::int64_t abs) -> std::optional<std::vector<std::byte>> {
        if (!frag_ok(abs)) return std::nullopt;
        try {
            return disk_.read_bytes(fs_.partition_offset_bytes() + static_cast<std::uint64_t>(abs) * sb_.fragment_size, static_cast<std::size_t>(sb_.block_size));
        } catch (...) {
            return std::nullopt;
        }
    };

    // direct data blocks
    for (std::int64_t b = 0; b < 12 && b < nblocks; ++b) {
        const std::int64_t d = in.direct_blocks[static_cast<std::size_t>(b)];
        if (d == 0) break;
        int frags_this = fpb;
        if (b == nblocks - 1 && !needs_indirect) {
            const std::int64_t tail = in.size - b * sb_.block_size;
            frags_this = static_cast<int>((tail + sb_.fragment_size - 1) / sb_.fragment_size);
            frags_this = std::max(1, std::min(frags_this, fpb));
        }
        if (frag_ok(d)) free_block_run(d, frags_this);
    }

    // single indirect
    // free the data blocks it points to, then the block itself
    if (in.indirect_block != 0) {
        if (auto blk = try_read(in.indirect_block))
            for (std::int64_t j = 0; j < ppb; ++j)
                free_ptr(static_cast<std::int64_t>(ps3hdd::read_be_u64(blk->data() + j * 8)));
        free_ptr(in.indirect_block);
    }

    // Double indirect.
    if (in.double_indirect_block != 0) {
        if (auto dbl = try_read(in.double_indirect_block))
            for (std::int64_t l1 = 0; l1 < ppb; ++l1) {
                const std::int64_t l1p = static_cast<std::int64_t>(ps3hdd::read_be_u64(dbl->data() + l1 * 8));
                if (l1p == 0) continue;
                if (auto l1blk = try_read(l1p))
                    for (std::int64_t j = 0; j < ppb; ++j)
                        free_ptr(static_cast<std::int64_t>(ps3hdd::read_be_u64(l1blk->data() + j * 8)));
                free_ptr(l1p);
            }
        free_ptr(in.double_indirect_block);
    }
}

std::uint64_t ufs2_writer::remove_entry_from_directory(std::uint64_t parent_inode, const std::string& name) {
    const auto parent = fs_.read_inode(parent_inode);
    const std::size_t block_bytes = static_cast<std::size_t>(sb_.block_size);
    for (int i = 0; i < 12; ++i) {
        const std::int64_t frag = parent.direct_blocks[i];
        if (frag == 0) break;
        const std::uint64_t off = fs_.partition_offset_bytes() +
            static_cast<std::uint64_t>(frag) * sb_.fragment_size;
        auto block = disk_.read_bytes(off, block_bytes);

        constexpr int DIRBLKSIZ = 512;
        const int sections = static_cast<int>(block.size()) / DIRBLKSIZ;
        for (int s = 0; s < sections; ++s) {
            const int start = s * DIRBLKSIZ;
            int prev = -1, offset = start;
            while (offset < start + DIRBLKSIZ) {
                const std::uint32_t ino = ps3hdd::read_be_u32(block.data() + offset);
                const std::uint16_t rec = ps3hdd::read_be_u16(block.data() + offset + 4);
                if (rec == 0) break;
                const std::uint8_t nl = std::to_integer<std::uint8_t>(block[offset + 7]);
                if (ino != 0 && nl == name.size() &&
                    std::memcmp(block.data() + offset + 8, name.data(), nl) == 0) {
                    if (prev >= 0) {
                        const std::uint16_t pr = ps3hdd::read_be_u16(block.data() + prev + 4);
                        ps3hdd::write_be_u16(block.data() + prev + 4, static_cast<std::uint16_t>(pr + rec));
                    } else {
                        ps3hdd::write_be_u32(block.data() + offset, 0);
                    }
                    write_data_block(frag, block);
                    return ino;
                }
                prev = offset;
                offset += rec;
            }
        }
    }
    throw std::runtime_error("entry not found in directory");
}

void ufs2_writer::truncate_directory_tail(std::uint64_t dir_inode) {
    if (!use_lv2_policy_) return;
    const int fpb = frags_per_block();
    const std::size_t block_bytes = static_cast<std::size_t>(sb_.block_size);

    const std::int64_t pg = static_cast<std::int64_t>(dir_inode) / sb_.inodes_per_group;
    const std::int64_t pidx = static_cast<std::int64_t>(dir_inode) % sb_.inodes_per_group;
    const std::uint64_t ioff = fs_.partition_offset_bytes() +
        static_cast<std::uint64_t>(pg) * sb_.frags_per_group * sb_.fragment_size +
        sb_.inode_block_offset * sb_.fragment_size + static_cast<std::uint64_t>(pidx) * superblock::inode_size;
    auto raw = disk_.read_bytes(ioff, superblock::inode_size);

    auto block_has_entries = [&](std::int64_t frag, std::size_t extent) {
        auto blk = disk_.read_bytes(fs_.partition_offset_bytes() + static_cast<std::uint64_t>(frag) * sb_.fragment_size, extent);
        for (std::size_t o = 0; o + 8 <= blk.size();) {
            const std::uint32_t ino = ps3hdd::read_be_u32(blk.data() + o);
            const std::uint16_t rec = ps3hdd::read_be_u16(blk.data() + o + 4);
            if (rec == 0) break;
            if (ino != 0) return true;
            o += rec;
        }
        return false;
    };

    bool changed = false;

    for (int i = 11; i >= 1; --i) {
        const std::int64_t frag = static_cast<std::int64_t>(ps3hdd::read_be_u64(raw.data() + 0x70 + i * 8));
        if (frag == 0) continue;
        if (block_has_entries(frag, block_bytes)) break;
        free_block_run(frag, fpb);
        ps3hdd::write_be_u64(raw.data() + 0x70 + i * 8, 0);
        const std::uint64_t blocks = ps3hdd::read_be_u64(raw.data() + 0x18);
        const std::uint64_t used = static_cast<std::uint64_t>(block_bytes) / 512;
        ps3hdd::write_be_u64(raw.data() + 0x18, blocks > used ? blocks - used : 0);
        ps3hdd::write_be_u64(raw.data() + 0x10, static_cast<std::uint64_t>(i) * block_bytes);
        changed = true;
    }
    if (changed) write_inode(dir_inode, raw);
}

void ufs2_writer::delete_file(std::uint64_t parent_inode, const std::string& name) {
    const std::uint64_t child = remove_entry_from_directory(parent_inode, name);
    const auto in = fs_.read_inode(child);
    if (in.is_directory())
        throw std::runtime_error("delete_file: entry is a directory");

    free_inode_blocks(in);
    const int cg_num = static_cast<int>(child / sb_.inodes_per_group);
    auto& cg = read_cylinder_group(cg_num);
    mark_inode_free(cg, static_cast<int>(child % sb_.inodes_per_group));
    write_cylinder_group(cg);
    truncate_directory_tail(parent_inode);
}

void ufs2_writer::delete_directory(std::uint64_t parent_inode, const std::string& name) {
    const auto parent = fs_.read_inode(parent_inode);
    std::uint64_t child = 0;
    for (const auto& e : fs_.read_directory(parent))
        if (e.name == name) { child = e.inode_number; break; }
    if (child == 0) throw std::runtime_error("directory entry not found");

    const auto dir = fs_.read_inode(child);
    if (!dir.is_directory()) throw std::runtime_error("delete_directory: entry is not a directory");
    for (const auto& e : fs_.read_directory(dir))
        if (e.name != "." && e.name != "..")
            throw std::runtime_error("delete_directory: directory is not empty");

    remove_entry_from_directory(parent_inode, name);
    free_inode_blocks(dir);
    const int cg_num = static_cast<int>(child / sb_.inodes_per_group);
    auto& cg = read_cylinder_group(cg_num);
    mark_inode_free(cg, static_cast<int>(child % sb_.inodes_per_group));
    put_i32(cg.raw_data, 0x18, get_i32(cg.raw_data, 0x18) - 1); // one fewer directory
    write_cylinder_group(cg);

    const std::int64_t pg = static_cast<std::int64_t>(parent_inode) / sb_.inodes_per_group;
    const std::int64_t pidx = static_cast<std::int64_t>(parent_inode) % sb_.inodes_per_group;
    const std::uint64_t p_off = fs_.partition_offset_bytes() +
        static_cast<std::uint64_t>(pg) * sb_.frags_per_group * sb_.fragment_size +
        sb_.inode_block_offset * sb_.fragment_size + static_cast<std::uint64_t>(pidx) * superblock::inode_size;
    auto praw = disk_.read_bytes(p_off, superblock::inode_size);
    const std::uint16_t nlink = ps3hdd::read_be_u16(praw.data() + 0x02);
    if (nlink > 0) put_u16(praw, 0x02, static_cast<std::uint16_t>(nlink - 1));
    write_inode(parent_inode, praw);
}


void ufs2_writer::delete_tree(std::uint64_t parent_inode, const std::string& name) {
    const auto parent = fs_.read_inode(parent_inode);
    std::uint64_t top = 0;
    for (const auto& e : fs_.read_directory(parent))
        if (e.name == name) { top = e.inode_number; break; }
    if (top == 0) throw std::runtime_error("delete_tree: entry not found");

    if (!fs_.read_inode(top).is_directory()) {
        delete_file(parent_inode, name);
        return;
    }
    struct frame {
        std::uint64_t parent;
        std::string name;
        std::uint64_t inode;
        bool exit; // true = children done, remove this now empty dir
    };
    std::vector<frame> stack;
    std::set<std::uint64_t> seen;
    stack.push_back({parent_inode, name, top, false});
    while (!stack.empty()) {
        const frame f = stack.back();
        stack.pop_back();
        if (f.exit) { delete_directory(f.parent, f.name); continue; }
        const auto in = fs_.read_inode(f.inode);
        if (!in.is_directory()) { delete_file(f.parent, f.name); continue; }
        if (!seen.insert(f.inode).second) { delete_directory(f.parent, f.name); continue; }
        stack.push_back({f.parent, f.name, f.inode, true}); // delete after its children
        for (const auto& e : fs_.read_directory(in))
            if (e.name != "." && e.name != "..")
                stack.push_back({f.inode, e.name, e.inode_number, false});
    }
}

void ufs2_writer::move_entry(std::uint64_t src_parent, const std::string& old_name, std::uint64_t dest_parent, const std::string& new_name) {
    // find the entry in the source directory
    const auto src = fs_.read_inode(src_parent);
    std::uint64_t child = 0;
    for (const auto& e : fs_.read_directory(src))
        if (e.name == old_name) { child = e.inode_number; break; }
    if (child == 0) throw std::runtime_error("move: source entry not found");

    if (directory_contains_entry(dest_parent, new_name))
        throw std::runtime_error("move: destination already contains the name");

    const auto child_inode = fs_.read_inode(child);
    const bool is_dir = child_inode.is_directory();
    const std::uint8_t et = is_dir ? std::uint8_t{4} : std::uint8_t{8}; // DT_DIR / DT_REG

    add_entry_to_directory(dest_parent, child, new_name, et);
    remove_entry_from_directory(src_parent, old_name);

    if (is_dir && src_parent != dest_parent) {
        const std::int64_t frag = child_inode.direct_blocks[0];
        if (frag != 0) {
            std::array<std::byte, 4> dd{};
            ps3hdd::write_be_u32(dd.data(), static_cast<std::uint32_t>(dest_parent));
            patch_bytes(fs_.partition_offset_bytes() + static_cast<std::uint64_t>(frag) * sb_.fragment_size + 12, dd);
        }
        // A subdirectory leaving/arriving changes each parent's link count.
        auto adjust_nlink = [&](std::uint64_t inode_number, int delta) {
            const std::int64_t g = static_cast<std::int64_t>(inode_number) / sb_.inodes_per_group;
            const std::int64_t ix = static_cast<std::int64_t>(inode_number) % sb_.inodes_per_group;
            const std::uint64_t off = fs_.partition_offset_bytes() +
                static_cast<std::uint64_t>(g) * sb_.frags_per_group * sb_.fragment_size +
                sb_.inode_block_offset * sb_.fragment_size + static_cast<std::uint64_t>(ix) * superblock::inode_size;
            auto raw = disk_.read_bytes(off, superblock::inode_size);
            const int nlink = static_cast<std::int16_t>(ps3hdd::read_be_u16(raw.data() + 0x02)) + delta;
            ps3hdd::write_be_u16(raw.data() + 0x02, static_cast<std::uint16_t>(nlink));
            write_inode(inode_number, raw);
        };
        adjust_nlink(src_parent, -1);
        adjust_nlink(dest_parent, +1);
    }
}

} // namespace ps3hdd::fs