#include "../common.h"

#include <ps3hdd_app/gameos.h>
#include <ps3hdd_disk/disk_source.h>
#include <ps3hdd_disk/physical_disk_source.h>
#include <ps3hdd_disk/raw_device.h>
#include <ps3hdd_fs/ufs2_checker.h>
#include <ps3hdd_fs/ufs2_filesystem.h>

#include <cstdint>
#include <cstdio>
#include <exception>
#include <memory>
#include <string>

using namespace ps3hdd;
using ps3hdd::tools::parse_hex;

int main(int argc, char** argv) {
    if (argc < 3) { std::printf("usage: ps3hdd_fsck <device|image> <eid_key_hex>\n"); return 1; }
    const std::string device = argv[1];
    try {
        const auto eid = parse_hex(argv[2]);
        if (eid.size() != 48) { std::printf("EID Root Key must be 48 bytes\n"); return 1; }

        auto raw = disk::open_raw_device(device, /*writable=*/false);
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
        std::printf("  cg_block_off=%lld data_block_off=%lld total_frags=%lld\n\n", (long long)sb.cg_block_offset, (long long)sb.data_block_offset, (long long)sb.total_fragments);

        std::printf("== WALKING TREE ==\n");
        const auto rep = fs::check_consistency(ufs, *m->decrypted);
        std::printf("Walked %lld inodes, %lld fragments claimed.\n", (long long)rep.inodes_walked, (long long)rep.fragments_claimed);
        std::printf("cross_links: %lld   out_of_range: %lld   used_but_free: %lld\n", (long long)rep.cross_links, (long long)rep.out_of_range, (long long)rep.used_but_free);
        for (const auto& f : rep.findings) std::printf("  %s\n", f.c_str());

        std::printf("\n== RESULT ==\n" "  cross_links=%lld  out_of_range=%lld  used_but_free=%lld  summary-mismatches=%lld\n", (long long)rep.cross_links, (long long)rep.out_of_range, (long long)rep.used_but_free, (long long)rep.summary_mismatches);
        std::printf(rep.clean() ? "  CLEAN - filesystem is globally consistent.\n" : "  INCONSISTENT - see findings above!\n");
        return 0;
    } catch (const std::exception& ex) {
        std::printf("\nERROR: %s\n", ex.what());
        return 1;
    }
}