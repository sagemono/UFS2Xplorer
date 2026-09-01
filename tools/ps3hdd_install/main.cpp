#include "../common.h"

#include <ps3hdd_app/gameos.h>
#include <ps3hdd_disk/disk_source.h>
#include <ps3hdd_disk/physical_disk_source.h>
#include <ps3hdd_disk/raw_device.h>
#include <ps3hdd_fs/ufs2_checker.h>
#include <ps3hdd_fs/ufs2_filesystem.h>
#include <ps3hdd_app/database.h>
#include <ps3hdd_fs/ufs2_writer.h>
#include <ps3hdd_pkg/pkg_installer.h>
#include <ps3hdd_pkg/ps3_pkg_reader.h>

#include <cstdint>
#include <cstdio>
#include <exception>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

using namespace ps3hdd;
using ps3hdd::tools::parse_hex;

namespace {
std::string human(std::uint64_t b) { return disk::format_size(b); }
} // namespace

int main(int argc, char** argv) {
    if (argc < 4) {
        std::printf("usage: ps3hdd_install <device> <eid_key_hex> <file.pkg> [--write] [--rebuild-db] [--lv2-policy]\n"
                    "  --rebuild-db   after install, clear mms/db so the console rebuilds the game\n"
                    "                 list on next boot (no Safe Mode step needed)\n"
                    "  --lv2-policy   EXPERIMENTAL. lv2's own ffs_dirpref/ffs_blkpref placement.\n"
                    "                 Verified on hardware: files read back correctly and the\n"
                    "                 console boots and launches the game (after Rebuild Database).\n"
                    "                 Not yet proven equivalent to what the console itself writes.\n"
                    "                 Always run ps3hdd_fsck afterwards!\n");
        return 1;
    }
    const std::string device = argv[1];
    bool do_write = false, do_recalc = false, do_rebuild_db = false, do_lv2_policy = false;
    std::string uninstall_title, pkg_path;
    for (int i = 3; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--write") do_write = true;
        else if (a == "--recalc") do_recalc = true;
        else if (a == "--rebuild-db") do_rebuild_db = true;
        else if (a == "--lv2-policy") do_lv2_policy = true;
        else if (a == "--uninstall" && i + 1 < argc) uninstall_title = argv[++i];
        else if (pkg_path.empty()) pkg_path = a;
    }

    std::printf("== ps3hdd_install %s ==\nDevice: %s\nPackage: %s\n\n", do_write ? "(WRITE)" : "(DRY RUN)", device.c_str(), pkg_path.c_str());

    try {
        const auto eid = parse_hex(argv[2]);
        if (eid.size() != 48) { std::printf("EID Root Key must be 48 bytes\n"); return 1; }

        auto raw = disk::open_raw_device(device, /*writable=*/do_write);
        auto src = std::make_shared<disk::physical_disk_source>(std::move(raw));
        std::printf("Disk: %s\n", src->description().c_str());

        auto m = app::open_gameos(src, {eid.data(), eid.size()});
        if (!m) { std::printf("Could not locate/mount the GameOS partition.\n"); return 1; }
        std::printf("GameOS mounted at sector 0x%llx.\n\n", static_cast<unsigned long long>(m->partition_sector));

        // RECALC mode: rebuild fs_cstotal + the CS summary table from the CGs
        if (do_recalc) {
            fs::ufs2_filesystem ufs(*m->decrypted, m->partition_sector);
            ufs.mount();
            std::printf("Free space (stored): %s\n", human(static_cast<std::uint64_t>(ufs.sb().free_space_bytes())).c_str());
            if (!do_write) { std::printf("[dry run] re-run with --write to rewrite the summary.\n"); return 0; }
            fs::ufs2_writer writer(ufs, *m->decrypted);
            writer.update_superblock();
            fs::ufs2_filesystem re(*m->decrypted, m->partition_sector);
            re.mount();
            std::printf("Recomputed. Free space (recalculated): %s\n", human(static_cast<std::uint64_t>(re.sb().free_space_bytes())).c_str());
            return 0;
        }

        // Uninstall mode: recursively delete game/<title>.
        if (!uninstall_title.empty()) {
            fs::ufs2_filesystem ufs(*m->decrypted, m->partition_sector);
            ufs.mount();
            const auto game = ufs.resolve_path("game");
            if (!game) { std::printf("game/ not found\n"); return 1; }
            std::printf("%s game/%s/ ...\n", do_write ? "Deleting" : "[dry run] would delete", uninstall_title.c_str());
            if (!do_write) { std::printf("Re-run with --write to actually delete.\n"); return 0; }
            fs::ufs2_writer writer(ufs, *m->decrypted);
            const auto game_num = ufs.resolve_path_to_inode_number("game");
            writer.delete_tree(*game_num, uninstall_title);
            writer.update_superblock();
            std::printf("Removed game/%s/.\n", uninstall_title.c_str());
            return 0;
        }

        if (pkg_path.empty()) { std::printf("no package given\n"); return 1; }
        auto pkg = pkg::ps3_pkg_reader::from_file(pkg_path);
        std::uint64_t total = 0; int files = 0;
        for (const auto& e : pkg.entries()) if (!e.is_directory) { ++files; total += e.data_size; }
        std::printf("Package: %s  (title %s)\n  %d files, %s\n  install -> game/%s/\n\n", pkg.content_id().c_str(), pkg.title_id().c_str(), files, human(total).c_str(), pkg.title_id().c_str());

        fs::ufs2_filesystem ufs(*m->decrypted, m->partition_sector);
        ufs.mount();
        std::printf("Free space before: %s\n", human(static_cast<std::uint64_t>(ufs.sb().free_space_bytes())).c_str());

        if (!do_write) {
            std::printf("\nDRY RUN complete. Re-run with --write to install for real.\n");
            return 0;
        }

        std::printf("\n*** WRITING to game/%s/ ***\n", pkg.title_id().c_str());
        fs::ufs2_writer writer(ufs, *m->decrypted);
        if (do_lv2_policy) {
            std::printf("\nNote: --lv2-policy is EXPERIMENTAL. It has been hardware tested, but it is not yet\n proven to match what the console itself writes. Run ps3hdd_fsck afterwards.\n\n");
            if (writer.set_lv2_policy(true))
                std::printf("lv2 placement policy: ON (ffs_dirpref + ffs_hashalloc, cs array loaded)\n");
            else
                std::printf("lv2 placement policy: REQUESTED BUT UNAVAILABLE - this filesystem has no\n  usable cs summary array, so the normal allocator is being used instead.\n");
        }
        pkg::pkg_installer installer(ufs, writer, pkg);
        const std::string path = installer.install([&](const std::string& name, int done, int tot) {
            std::printf("  [%d/%d] %s\r", done, tot, name.c_str());
            std::fflush(stdout);
        });
        std::printf("\nInstalled to %s.\n", path.c_str());

        {
            const auto ipg = ufs.sb().inodes_per_group;
            std::printf("\n== DIRECTORY PLACEMENT (inode -> cylinder group) ==\n");
            std::function<void(std::uint64_t, const std::string&, int)> show =
                [&](std::uint64_t ino, const std::string& name, int depth) {
                    if (depth > 4) return;
                    fs::inode in;
                    try { in = ufs.read_inode(ino); } catch (const std::exception&) { return; }
                    if (!in.is_directory()) return;
                    std::printf("  %*s%-24s inode %-8llu cg %lld\n", depth * 2, "", name.c_str(), (unsigned long long)ino, (long long)(ipg > 0 ? ino / ipg : 0));
                    for (const auto& e : ufs.read_directory(in)) {
                        if (e.name == "." || e.name == "..") continue;
                        show(e.inode_number, e.name, depth + 1);
                    }
                };
            if (const auto root = ufs.resolve_path_to_inode_number(path)) show(*root, path, 0);
        }

        if (do_rebuild_db) {
            const int n = app::invalidate_content_database(ufs, writer);
            if (n < 0) std::printf("Note: mms/db not found; content database not cleared.\n");
            else std::printf("Cleared %d content database entries; the console rebuilds on next boot.\n", n);
        }

        // read every file back through the reader decrypted view and compare it to the package
        std::printf("Verifying ...\n");
        fs::ufs2_filesystem check(*m->decrypted, m->partition_sector);
        check.mount();
        int ok = 0, bad = 0;
        for (const auto& e : pkg.entries()) {
            if (e.is_directory) continue;
            const auto inode = check.resolve_path(path + "/" + e.name);
            if (!inode) { std::printf("  MISSING %s\n", e.name.c_str()); ++bad; continue; }
            auto on_disk = check.read_inode_data(*inode);
            std::vector<std::byte> expected(e.data_size);
            pkg.decrypt_range(e, 0, expected);
            if (on_disk == expected) ++ok;
            else { std::printf("  MISMATCH %s\n", e.name.c_str()); ++bad; }
        }
        std::printf("Verify: %d ok, %d bad.\n", ok, bad);
        if (bad != 0) { std::printf("\nFAILED! see mismatches above.\n"); return 2; }

        std::printf("Checking filesystem consistency ...\n");
        auto rep = fs::check_consistency(check, *m->decrypted);
        std::printf("  %lld inodes, %lld fragments; %s\n", (long long)rep.inodes_walked, (long long)rep.fragments_claimed, rep.summary_line().c_str());
        if (!rep.structurally_damaged() && rep.repairable()) {
            std::printf("  repairing accounting drift ...\n");
            if (!rep.used_but_free_frags.empty()) writer.repair_used_but_free(rep.used_but_free_frags);
            writer.repair_free_counts({});
            fs::ufs2_filesystem re(*m->decrypted, m->partition_sector);
            re.mount();
            rep = fs::check_consistency(re, *m->decrypted);
            std::printf("  after repair: %s\n", rep.summary_line().c_str());
        }
        if (!rep.safe_to_write()) {
            for (const auto& f : rep.findings) std::printf("  %s\n", f.c_str());
            std::printf("\nFAILED! filesystem is INCONSISTENT after install. Do NOT boot this disk!! \n restore it (Safe Mode -> Restore PS3 System) and create an issue ASAP\n");
            return 3;
        }
        if (rep.orphan_inodes > 0)
            std::printf("  note: %lld pre-existing orphan inode(s) from console-side deletes;\n        ps3hdd_fsck --reclaim-orphans recovers the space.\n", (long long)rep.orphan_inodes);
        std::printf("\nSUCCESS! every file verified and the filesystem is globally consistent.\n");
        return 0;
    } catch (const std::exception& ex) {
        std::printf("\nERROR: %s\n", ex.what());
        return 1;
    }
}