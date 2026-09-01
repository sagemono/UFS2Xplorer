#include "../common.h"

#include <ps3hdd_app/gameos.h>
#include <ps3hdd_disk/disk_source.h>
#include <ps3hdd_disk/physical_disk_source.h>
#include <ps3hdd_disk/raw_device.h>
#include <ps3hdd_fs/ufs2_checker.h>
#include <ps3hdd_fs/ufs2_filesystem.h>
#include <ps3hdd_fs/ufs2_writer.h>

#include <cstdint>
#include <cstdio>
#include <exception>
#include <memory>
#include <string>

using namespace ps3hdd;
using ps3hdd::tools::parse_hex;

namespace { std::string human(std::uint64_t b) { return ps3hdd::disk::format_size(b); } }

int main(int argc, char** argv) {
    if (argc < 3) {
        std::printf("usage: ps3hdd_fsck <device|image> <eid_key_hex> [--fix] [--reclaim-orphans]\n"
                    "  --fix              repair 'used-but-free' fragments: mark blocks that a live\n"
                    "                     file references back as allocated, then recompute the free\n"
                    "                     counters. Only moves a bit free -> used; cannot lose data.\n"
                    "  --reclaim-orphans  DESTRUCTIVE. Free inodes that are marked used but are\n"
                    "                     unreachable from the root, and release their data blocks.\n"
                    "                     Recovers leaked space, but anything the tree walk failed to\n"
                    "                     reach is destroyed. Run --fix first and read the list.\n");
        return 1;
    }
    const std::string device = argv[1];
    bool do_fix = false, do_reclaim = false;
    for (int i = 3; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--fix") do_fix = true;
        else if (a == "--reclaim-orphans") do_reclaim = true;
    }
    try {
        const auto eid = parse_hex(argv[2]);
        if (eid.size() != 48) { std::printf("EID Root Key must be 48 bytes\n"); return 1; }

        auto raw = disk::open_raw_device(device, /*writable=*/(do_fix || do_reclaim));
        auto src = std::make_shared<disk::physical_disk_source>(std::move(raw));
        std::printf("Disk: %s\n", src->description().c_str());

        auto m = app::open_gameos(src, {eid.data(), eid.size()});
        if (!m) { std::printf("Could not locate/mount the GameOS partition.\n"); return 1; }

        fs::ufs2_filesystem ufs(*m->decrypted, m->partition_sector);
        if (!ufs.mount()) { std::printf("mount failed\n"); return 1; }
        const auto& sb = ufs.sb();
        std::printf("\n== GEOMETRY (partition sector 0x%llx) ==\n", (unsigned long long)m->partition_sector);
        std::printf("  block_size=%lld frag_size=%lld frags/block=%lld\n", (long long)sb.block_size, (long long)sb.fragment_size, (long long)(sb.block_size / sb.fragment_size));
        std::printf("  ncg=%d frags/group=%lld inodes/group=%lld\n", sb.cylinder_groups, (long long)sb.frags_per_group, (long long)sb.inodes_per_group);
        std::printf("  cg_block_off=%lld data_block_off=%lld total_frags=%lld\n", (long long)sb.cg_block_offset, (long long)sb.data_block_offset, (long long)sb.total_fragments);
        {

            const std::int64_t raw_free = sb.free_space_bytes();
            const std::int64_t reserve = sb.total_data_fragments > 0 && sb.min_free_percent > 0 ? sb.total_data_fragments * sb.fragment_size / 100 * sb.min_free_percent : 0;
            const std::int64_t usable = raw_free > reserve ? raw_free - reserve : 0;
            if (sb.min_free_percent != 8 || sb.optim != 0)
                std::printf("  NOTE: minfree=%d%% optim=%d - this disk has been patched by the\n        'unlock HDD space' homebrew (stock PS3 is minfree=8%% optim=0).\n", sb.min_free_percent, sb.optim);
            std::printf("  free: %s raw", human(static_cast<std::uint64_t>(raw_free)).c_str());
            if (reserve > 0)
                std::printf("  |  minfree=%d%% reserves %s  ->  ~%s as the console reports it", sb.min_free_percent, human(static_cast<std::uint64_t>(reserve)).c_str(), human(static_cast<std::uint64_t>(usable)).c_str());
            std::printf("\n\n");
        }

        std::printf("== WALKING TREE ==\n");
        const auto rep = fs::check_consistency(ufs, *m->decrypted);
        std::printf("Walked %lld inodes, %lld fragments claimed.\n", (long long)rep.inodes_walked, (long long)rep.fragments_claimed);
        std::printf("cross_links: %lld   out_of_range: %lld   used_but_free: %lld\n", (long long)rep.cross_links, (long long)rep.out_of_range, (long long)rep.used_but_free);
        for (const auto& f : rep.findings) std::printf("  %s\n", f.c_str());

        std::printf("\n== RESULT ==\n" "  cross_links=%lld  out_of_range=%lld  used_but_free=%lld  summary-mismatches=%lld\n", (long long)rep.cross_links, (long long)rep.out_of_range, (long long)rep.used_but_free, (long long)rep.summary_mismatches);
        std::printf("  cluster-map-mismatches=%lld  cluster-sum-mismatches=%lld\n",
                    (long long)rep.cluster_map_mismatches, (long long)rep.cluster_sum_mismatches);
        std::printf("  orphan-inodes=%lld (reclaimable=%lld, still-linked=%lld)  cs-array-mismatches=%lld  cstotal-mismatches=%lld\n", (long long)rep.orphan_inodes, (long long)rep.orphan_inodes_unlinked, (long long)rep.orphan_inodes_linked, (long long)rep.cs_array_mismatches, (long long)rep.cstotal_mismatches);
        if (rep.clean()) std::printf("  CLEAN - filesystem is globally consistent.\n");
        else if (rep.structurally_damaged()) std::printf("  DAMAGED - a fragment is claimed twice or points outside the filesystem.\n            --fix cannot resolve this; restore the disk on the console.\n");
        else if (rep.repairable()) std::printf("  INCONSISTENT - accounting disagrees with the bitmaps; re-run with --fix.\n");
        else std::printf("  CLEAN - consistent; only leaked (orphan) inodes remain, see below.\n");

        if (rep.orphan_inodes > 0 && !do_reclaim) {
            std::printf("\n  Orphan inodes usually come from deleting a game on the console: the PS3\n"
                        "  unlinks it immediately but frees the inodes and blocks asynchronously\n"
                        "  (soft updates), so powering off first can leave them allocated. Re-run\n"
                        "  with --reclaim-orphans to release them and recover the space.\n");
        }

        if (do_reclaim && !rep.orphan_all_inodes.empty()) {
            std::printf("\n== RECLAIMING %zu orphan inode(s) ==\n", rep.orphan_all_inodes.size());
            std::printf("  (%lld reclaimable, %lld still claiming links - ALL will be freed)\n", (long long)rep.orphan_inodes_unlinked, (long long)rep.orphan_inodes_linked);
            const auto free_before = ufs.sb().free_space_bytes();

            std::printf("  building live-block map ...\n");
            const auto claimed = fs::claimed_fragment_map(ufs, *m->decrypted);
            fs::ufs2_writer rw(ufs, *m->decrypted);
            const int n = rw.reclaim_orphan_inodes(rep.orphan_all_inodes, claimed);
            rw.repair_free_counts({});
            rw.update_superblock();
            fs::ufs2_filesystem re2(*m->decrypted, m->partition_sector);
            re2.mount();
            std::printf("  freed %d inode(s); free space %s -> %s\n", n, human(static_cast<std::uint64_t>(free_before)).c_str(), human(static_cast<std::uint64_t>(re2.sb().free_space_bytes())).c_str());
            const auto after = fs::check_consistency(re2, *m->decrypted);
            std::printf("\n== AFTER RECLAIM ==\n  cross_links=%lld  used_but_free=%lld  orphan-inodes=%lld  summary-mismatches=%lld\n", (long long)after.cross_links, (long long)after.used_but_free, (long long)after.orphan_inodes, (long long)after.summary_mismatches);
            std::printf(after.clean() ? "  CLEAN - reclaim succeeded.\n" : "  STILL INCONSISTENT - see above.\n");
            return 0;
        }

        if (do_fix) {
            if (rep.used_but_free_frags.empty() && rep.cluster_map_mismatches == 0 &&
                rep.cluster_sum_mismatches == 0 && rep.summary_mismatches == 0) {
                std::printf("\n--fix: nothing to repair.\n");
                return 0;
            }
            std::printf("\n== REPAIRING %zu used-but-free fragment(s) ==\n", rep.used_but_free_frags.size());
            fs::ufs2_writer writer(ufs, *m->decrypted);
            const int fixed = writer.repair_used_but_free(rep.used_but_free_frags);
            std::printf("  marked %d fragment(s) as allocated\n", fixed);
            const int cgs = writer.repair_free_counts({});
            std::printf("  recomputed free counts in %d cylinder group(s)\n", cgs);
            writer.update_superblock();

            fs::ufs2_filesystem re(*m->decrypted, m->partition_sector);
            re.mount();
            const auto after = fs::check_consistency(re, *m->decrypted);
            std::printf("\n== AFTER REPAIR ==\n  cross_links=%lld  out_of_range=%lld  used_but_free=%lld  summary-mismatches=%lld\n", (long long)after.cross_links, (long long)after.out_of_range, (long long)after.used_but_free, (long long)after.summary_mismatches);
            std::printf("  orphan-inodes=%lld  cs-array-mismatches=%lld  cstotal-mismatches=%lld\n", (long long)after.orphan_inodes, (long long)after.cs_array_mismatches, (long long)after.cstotal_mismatches);
            for (const auto& f : after.findings) std::printf("  %s\n", f.c_str());
            std::printf(after.clean() ? "  CLEAN - repair succeeded.\n" : "  STILL INCONSISTENT - do not boot; report this ASAP!\n");
        }
        return 0;
    } catch (const std::exception& ex) {
        std::printf("\nERROR: %s\n", ex.what());
        return 1;
    }
}