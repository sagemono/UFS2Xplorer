#include "ufs2_checker.h"

#include <ps3hdd_crypto/be_io.h>

#include <algorithm>
#include <cstdio>
#include <functional>
#include <utility>
#include <span>
#include <unordered_map>
#include <unordered_set>

namespace ps3hdd::fs {

namespace {

struct claim { std::uint64_t inode; bool metadata; };

class checker {
public:
    checker(ufs2_filesystem& fs, disk::disk_source& disk, std::size_t max_findings) : fs_(fs), disk_(disk), sb_(fs.sb()), max_findings_(max_findings), fpb_(static_cast<int>(sb_.block_size / sb_.fragment_size)), ppb_(sb_.block_size / 8) {}

    consistency_report run() {
        walk_dir(ufs2_filesystem::root_inode);
        check_bitmaps();
        check_cg_summaries();
        check_orphan_inodes();
        check_cs_array();
        report_.inodes_walked = static_cast<std::int64_t>(visited_.size());
        report_.fragments_claimed = static_cast<std::int64_t>(claims_.size());
        return std::move(report_);
    }

    std::vector<bool> claimed_map() {
        walk_dir(ufs2_filesystem::root_inode);
        std::vector<bool> m(static_cast<std::size_t>(std::max<std::int64_t>(sb_.total_fragments, 0)), false);
        for (const auto& [frag, c] : claims_) {
            (void)c;
            if (frag >= 0 && static_cast<std::size_t>(frag) < m.size()) m[static_cast<std::size_t>(frag)] = true;
        }
        return m;
    }

private:
    ufs2_filesystem& fs_;
    disk::disk_source& disk_;
    const superblock& sb_;
    std::size_t max_findings_;
    int fpb_;
    std::int64_t ppb_;

    std::unordered_set<std::uint64_t> visited_;
    std::unordered_map<std::int64_t, claim> claims_;
    consistency_report report_;

    void note(const std::string& msg) {
        if (report_.findings.size() < max_findings_) report_.findings.push_back(msg);
    }

    std::vector<std::byte> read_block(std::int64_t frag) {
        return disk_.read_bytes(fs_.partition_offset_bytes() + static_cast<std::uint64_t>(frag) * sb_.fragment_size, static_cast<std::size_t>(sb_.block_size));
    }

    void claim_frags(std::int64_t start, int count, std::uint64_t inum, bool meta) {
        for (int k = 0; k < count; ++k) {
            const std::int64_t f = start + k;
            if (f < sb_.data_block_offset || f >= sb_.total_fragments) {
                ++report_.out_of_range;
                note("out-of-range frag " + std::to_string(f) + " (inode " + std::to_string(inum) + ")");
                continue;
            }
            auto it = claims_.find(f);
            if (it != claims_.end()) {
                if (it->second.inode != inum) {
                    ++report_.cross_links;
                    note("cross-link frag " + std::to_string(f) + ": inode " + std::to_string(it->second.inode) + " and inode " + std::to_string(inum));
                }
            } else {
                claims_.emplace(f, claim{inum, meta});
            }
        }
    }

    void account_inode(const inode& in) {
        if (in.size <= 0) return;
        const std::int64_t nblocks = (in.size + sb_.block_size - 1) / sb_.block_size;
        std::int64_t dataidx = 0;
        auto tail_for = [&](std::int64_t idx) -> int {
            if (idx != nblocks - 1) return fpb_;
            const std::int64_t tail = in.size - idx * sb_.block_size;
            int f = static_cast<int>((tail + sb_.fragment_size - 1) / sb_.fragment_size);
            return f < 1 ? 1 : (f > fpb_ ? fpb_ : f);
        };
        auto emit_data = [&](std::int64_t ptr) {
            if (ptr != 0) claim_frags(ptr, tail_for(dataidx), in.number, false);
            ++dataidx;
        };
        for (int b = 0; b < 12; ++b) {
            if (in.direct_blocks[b] == 0) { ++dataidx; continue; }
            emit_data(in.direct_blocks[b]);
        }
        walk_indirect(in.indirect_block, 1, in.number, emit_data);
        walk_indirect(in.double_indirect_block, 2, in.number, emit_data);
        walk_indirect(in.triple_indirect_block, 3, in.number, emit_data);
    }

    void walk_indirect(std::int64_t iblk, int level, std::uint64_t inum, const std::function<void(std::int64_t)>& emit_data) {
        if (iblk == 0) return;
        claim_frags(iblk, fpb_, inum, true);
        auto blk = read_block(iblk);
        for (std::int64_t j = 0; j < ppb_; ++j) {
            const std::int64_t p = static_cast<std::int64_t>(ps3hdd::read_be_u64(blk.data() + j * 8));
            if (p == 0) continue;
            if (level == 1) emit_data(p);
            else walk_indirect(p, level - 1, inum, emit_data);
        }
    }

    void walk_dir(std::uint64_t dir_inode) {
        if (!visited_.insert(dir_inode).second) return;
        inode din = fs_.read_inode(dir_inode);
        account_inode(din);
        std::vector<directory_entry> ents;
        try { ents = fs_.read_directory(din); } catch (...) { return; }
        for (const auto& e : ents) {
            if (e.name == "." || e.name == ".." || e.inode_number == 0) continue;
            if (e.type == dirent_type::directory) {
                walk_dir(e.inode_number);
            } else {
                if (!visited_.insert(e.inode_number).second) continue;
                try { account_inode(fs_.read_inode(e.inode_number)); } catch (...) {}
            }
        }
    }

    std::pair<std::vector<std::byte>, int> read_cg(int cgn) {
        const std::uint64_t off = fs_.partition_offset_bytes() + static_cast<std::uint64_t>(cgn) * sb_.frags_per_group * sb_.fragment_size + static_cast<std::uint64_t>(sb_.cg_block_offset) * sb_.fragment_size;
        auto raw = disk_.read_bytes(off, static_cast<std::size_t>(sb_.cg_size));
        if (raw.size() < 0x64 || ps3hdd::read_be_u32(raw.data() + 0x04) != 0x00090255u)
            return {{}, 0};
        const int freeoff = static_cast<std::int32_t>(ps3hdd::read_be_u32(raw.data() + 0x60));
        return {std::move(raw), freeoff};
    }

    void check_bitmaps() {
        const std::int64_t fpg = sb_.frags_per_group;
        std::vector<std::pair<std::int64_t, std::uint64_t>> ordered;
        ordered.reserve(claims_.size());
        for (const auto& [frag, c] : claims_) ordered.emplace_back(frag, c.inode);
        std::sort(ordered.begin(), ordered.end());

        int cur_cg = -1;
        std::vector<std::byte> raw;
        int freeoff = 0;
        for (const auto& [frag, inum] : ordered) {
            const int cgn = static_cast<int>(frag / fpg);
            const int fic = static_cast<int>(frag % fpg);
            if (cgn < 0 || cgn >= sb_.cylinder_groups) continue;
            if (cgn != cur_cg) { std::tie(raw, freeoff) = read_cg(cgn); cur_cg = cgn; }
            if (raw.empty()) continue;
            const std::size_t byte = static_cast<std::size_t>(freeoff) + fic / 8;
            if (byte >= raw.size()) continue;
            const bool is_free = (std::to_integer<int>(raw[byte]) >> (fic % 8)) & 1;
            if (is_free) {
                ++report_.used_but_free;
                report_.used_but_free_frags.push_back(frag);
                note("used-but-free frag " + std::to_string(frag) + " (cg " + std::to_string(cgn) + "): inode " + std::to_string(inum) + " holds it but bitmap says free");
            }
        }
    }

    void check_orphan_inodes() {
        const int ipg = static_cast<int>(sb_.inodes_per_group);
        if (ipg <= 0) return;
        auto bit = [](const std::vector<std::byte>& d, int base, int idx) {
            const std::size_t b = static_cast<std::size_t>(base) + idx / 8;
            return b < d.size() && ((std::to_integer<int>(d[b]) >> (idx % 8)) & 1);
        };
        for (int cgn = 0; cgn < sb_.cylinder_groups; ++cgn) {
            auto [raw, freeoff] = read_cg(cgn);
            (void)freeoff;
            if (raw.empty()) continue;
            const int iusedoff = static_cast<int>(static_cast<std::int32_t>(ps3hdd::read_be_u32(raw.data() + 0x5C)));
            for (int i = 0; i < ipg; ++i) {
                if (!bit(raw, iusedoff, i)) continue;
                const std::uint64_t ino = static_cast<std::uint64_t>(cgn) * ipg + i;
                if (ino < 2) continue;
                if (visited_.count(ino)) continue;
                ++report_.orphan_inodes;
                report_.orphan_all_inodes.push_back(static_cast<std::int64_t>(ino));

                int nlink = -1;
                std::uint16_t mode = 0;
                try {
                    const auto raw = fs_.read_inode_raw(ino);
                    if (raw.size() >= 4) {
                        mode = ps3hdd::read_be_u16(raw.data() + 0x00);
                        nlink = static_cast<std::int16_t>(ps3hdd::read_be_u16(raw.data() + 0x02));
                    }
                } catch (const std::exception&) {
                }
                if (nlink == 0 || (nlink >= 0 && mode == 0)) {
                    ++report_.orphan_inodes_unlinked;
                    report_.orphan_unlinked_inodes.push_back(static_cast<std::int64_t>(ino));
                } else if (nlink > 0) {
                    ++report_.orphan_inodes_linked;
                }
                note("orphan inode " + std::to_string(ino) + " (cg " + std::to_string(cgn) + "): used in the bitmap but unreachable from root; mode=0" + std::to_string(mode) + " nlink=" + std::to_string(nlink) + (nlink == 0 || mode == 0 ? " [reclaimable]" : " [LINKED - lost data]"));
            }
        }
    }

    void check_cs_array() {
        const int ncg = sb_.cylinder_groups;
        if (ncg <= 0 || sb_.cs_address <= 0 || sb_.fragment_size <= 0) return;
        const std::size_t want = static_cast<std::size_t>(ncg) * 16;
        std::vector<std::byte> cs;
        try {
            cs = disk_.read_bytes(fs_.partition_offset_bytes() + static_cast<std::uint64_t>(sb_.cs_address) * sb_.fragment_size, want);
        } catch (const std::exception&) {
            return;
        }
        if (cs.size() < want) return;

        auto i32 = [](const std::vector<std::byte>& d, std::size_t o) {
            return static_cast<std::int64_t>(static_cast<std::int32_t>(ps3hdd::read_be_u32(d.data() + o)));
        };
        std::int64_t tot_ndir = 0, tot_nbfree = 0, tot_nifree = 0, tot_nffree = 0;
        for (int cgn = 0; cgn < ncg; ++cgn) {
            auto [raw, freeoff] = read_cg(cgn);
            (void)freeoff;
            if (raw.empty()) continue;
            const std::int64_t h_ndir = i32(raw, 0x18), h_nbfree = i32(raw, 0x1C), h_nifree = i32(raw, 0x20), h_nffree = i32(raw, 0x24);
            const std::size_t o = static_cast<std::size_t>(cgn) * 16;
            const std::int64_t c_ndir = i32(cs, o + 0), c_nbfree = i32(cs, o + 4), c_nifree = i32(cs, o + 8), c_nffree = i32(cs, o + 12);
            if (c_ndir != h_ndir || c_nbfree != h_nbfree || c_nifree != h_nifree || c_nffree != h_nffree) {
                ++report_.cs_array_mismatches;
                note("cs-array entry for cg " + std::to_string(cgn) + " disagrees with the cg header: cs(" + std::to_string(c_ndir) + "," + std::to_string(c_nbfree) + "," + std::to_string(c_nifree) + "," + std::to_string(c_nffree) + ") vs cg(" + std::to_string(h_ndir) + "," + std::to_string(h_nbfree) + "," + std::to_string(h_nifree) + "," + std::to_string(h_nffree) + ")");
            }
            tot_ndir += h_ndir; tot_nbfree += h_nbfree; tot_nifree += h_nifree; tot_nffree += h_nffree;
        }

        try {
            const auto sbd = disk_.read_bytes(fs_.partition_offset_bytes() + 65536, 8192);
            auto i64 = [](const std::vector<std::byte>& d, std::size_t o) {
                return static_cast<std::int64_t>(ps3hdd::read_be_u64(d.data() + o));
            };
            const std::int64_t s_ndir = i64(sbd, 0x3F0), s_nbfree = i64(sbd, 0x3F8), s_nifree = i64(sbd, 0x400), s_nffree = i64(sbd, 0x408);
            if (s_ndir != tot_ndir || s_nbfree != tot_nbfree || s_nifree != tot_nifree || s_nffree != tot_nffree) {
                ++report_.cstotal_mismatches;
                note("fs_cstotal disagrees with the sum over cylinder groups: sb(" + std::to_string(s_ndir) + "," + std::to_string(s_nbfree) + "," + std::to_string(s_nifree) + "," + std::to_string(s_nffree) + ") vs sum(" + std::to_string(tot_ndir) + "," + std::to_string(tot_nbfree) + "," + std::to_string(tot_nifree) + "," + std::to_string(tot_nffree) + ")");
            }
        } catch (const std::exception&) {
        }
    }

    void check_clusters(int cgn, const std::vector<std::byte>& raw, const std::vector<std::uint8_t>& want) {
        const int contigsum = sb_.contig_sum_size;
        if (contigsum <= 0) return;
        auto i32 = [](const std::vector<std::byte>& d, std::size_t o) {
            return static_cast<std::int64_t>(static_cast<std::int32_t>(ps3hdd::read_be_u32(d.data() + o)));
        };
        auto bit = [&](int base, int idx) {
            const std::size_t b = static_cast<std::size_t>(base) + idx / 8;
            return b < raw.size() && ((std::to_integer<int>(raw[b]) >> (idx % 8)) & 1);
        };
        const int clusteroff = static_cast<int>(i32(raw, 0x6C));
        const int clustersumoff = static_cast<int>(i32(raw, 0x68));
        int nclusterblks = static_cast<int>(i32(raw, 0x70));
        if (clusteroff <= 0 || clustersumoff <= 0 || nclusterblks <= 0) return;
        if (static_cast<std::size_t>(nclusterblks) > want.size())
            nclusterblks = static_cast<int>(want.size());
        std::int64_t bad_bits = 0;
        for (int b = 0; b < nclusterblks; ++b)
            if (bit(clusteroff, b) != (want[static_cast<std::size_t>(b)] != 0)) ++bad_bits;
        if (bad_bits) {
            report_.cluster_map_mismatches += bad_bits;
            note("cg " + std::to_string(cgn) + " cluster free-map: " + std::to_string(bad_bits) + " block(s) disagree with the fragment bitmap");
        }

        std::vector<std::int64_t> csum(static_cast<std::size_t>(contigsum) + 1, 0);
        int run = 0;
        for (int b = 0; b <= nclusterblks; ++b) {
            const bool free_here = b < nclusterblks && want[static_cast<std::size_t>(b)];
            if (free_here) { ++run; continue; }
            if (run > 0) ++csum[static_cast<std::size_t>(std::min(run, contigsum))];
            run = 0;
        }
        for (int i = 1; i <= contigsum; ++i) {
            const std::int64_t s = i32(raw, static_cast<std::size_t>(clustersumoff) + 4 * i);
            if (s != csum[static_cast<std::size_t>(i)]) {
                ++report_.cluster_sum_mismatches;
                note("cg " + std::to_string(cgn) + " clustersum[" + std::to_string(i) + "] stored " +
                     std::to_string(s) + " actual " + std::to_string(csum[static_cast<std::size_t>(i)]));
            }
        }
    }

    void check_cg_summaries() {
        const int fpb = fpb_;
        const int fpg = static_cast<int>(sb_.frags_per_group);
        const int ipg = static_cast<int>(sb_.inodes_per_group);
        auto i32 = [](const std::vector<std::byte>& d, std::size_t o) {
            return static_cast<std::int64_t>(static_cast<std::int32_t>(ps3hdd::read_be_u32(d.data() + o)));
        };
        auto bit = [](const std::vector<std::byte>& d, int base, int idx) {
            const std::size_t b = static_cast<std::size_t>(base) + idx / 8;
            return b < d.size() && ((std::to_integer<int>(d[b]) >> (idx % 8)) & 1);
        };
        for (int cgn = 0; cgn < sb_.cylinder_groups; ++cgn) {
            auto [raw, freeoff] = read_cg(cgn);
            if (raw.empty()) continue;
            const int iusedoff = static_cast<int>(i32(raw, 0x5C));

            std::int64_t nbfree = 0, nffree = 0;
            std::vector<std::int64_t> frsum(fpb, 0);
            std::vector<std::uint8_t> blockfree;
            blockfree.reserve(static_cast<std::size_t>(fpg / fpb));
            for (int base = 0; base + fpb <= fpg; base += fpb) {
                int freec = 0, run = 0;
                std::vector<int> runs;
                for (int f = 0; f < fpb; ++f) {
                    if (bit(raw, freeoff, base + f)) { ++freec; ++run; }
                    else if (run > 0) { runs.push_back(run); run = 0; }
                }
                if (run > 0) runs.push_back(run);
                blockfree.push_back(freec == fpb ? 1 : 0);
                if (freec == fpb) ++nbfree;
                else { nffree += freec; for (int r : runs) if (r >= 1 && r < fpb) ++frsum[r]; }
            }
            std::int64_t nifree = 0;
            for (int i = 0; i < ipg; ++i) if (!bit(raw, iusedoff, i)) ++nifree;

            const std::int64_t s_nbfree = i32(raw, 0x1C), s_nifree = i32(raw, 0x20), s_nffree = i32(raw, 0x24);
            if (s_nbfree != nbfree || s_nffree != nffree || s_nifree != nifree) {
                ++report_.summary_mismatches;
                note("cg " + std::to_string(cgn) + " counts stored(nbfree=" + std::to_string(s_nbfree) +
                     " nffree=" + std::to_string(s_nffree) + " nifree=" + std::to_string(s_nifree) +
                     ") actual(nbfree=" + std::to_string(nbfree) + " nffree=" + std::to_string(nffree) +
                     " nifree=" + std::to_string(nifree) + ")");
            }
            for (int i = 1; i < fpb; ++i) {
                const std::int64_t s = i32(raw, 0x34 + i * 4);
                if (s != frsum[i]) {
                    ++report_.summary_mismatches;
                    note("cg " + std::to_string(cgn) + " frsum[" + std::to_string(i) + "] stored " + std::to_string(s) + " actual " + std::to_string(frsum[i]));
                }
            }

            check_clusters(cgn, raw, blockfree);
        }
    }
};

} // namespace

std::string consistency_report::summary_line() const {
    char buf[512];
    std::snprintf(buf, sizeof buf,
                  "cross-links=%lld out-of-range=%lld used-but-free=%lld summary=%lld "
                  "cs-array=%lld cstotal=%lld cluster-map=%lld cluster-sum=%lld orphans=%lld",
                  (long long)cross_links, (long long)out_of_range, (long long)used_but_free,
                  (long long)summary_mismatches, (long long)cs_array_mismatches,
                  (long long)cstotal_mismatches, (long long)cluster_map_mismatches,
                  (long long)cluster_sum_mismatches, (long long)orphan_inodes);
    return buf;
}

consistency_report check_consistency(ufs2_filesystem& fs, disk::disk_source& disk, std::size_t max_findings) {
    checker c(fs, disk, max_findings);
    return c.run();
}

std::vector<bool> claimed_fragment_map(ufs2_filesystem& fs, disk::disk_source& disk) {
    checker c(fs, disk, 0);
    return c.claimed_map();
}

} // namespace ps3hdd::fs