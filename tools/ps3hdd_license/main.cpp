#include "../common.h"

#include <ps3hdd_app/gameos.h>
#include <ps3hdd_disk/disk_source.h>
#include <ps3hdd_disk/physical_disk_source.h>
#include <ps3hdd_disk/raw_device.h>
#include <ps3hdd_fs/ufs2_checker.h>
#include <ps3hdd_fs/ufs2_filesystem.h>
#include <ps3hdd_fs/ufs2_writer.h>
#include <ps3hdd_license/activation.h>
#include <ps3hdd_license/rap.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

using namespace ps3hdd;
using ps3hdd::tools::parse_hex;

namespace {

std::string content_id_from_path(const std::string& p) {
    std::size_t slash = p.find_last_of("/\\");
    std::string name = slash == std::string::npos ? p : p.substr(slash + 1);
    if (name.size() > 4 && name.substr(name.size() - 4) == ".rap") name = name.substr(0, name.size() - 4);
    return name;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 4) {
        std::printf("usage: ps3hdd_license <device> <eid_key_hex> <rap-file> [options]\n"
                    "  --content-id ID     override content id (default: RAP filename)\n"
                    "  --idps HEX          full offline activation: also generate act.dat + .rif\n"
                    "                      (16 byte console IDPS) into home/00000001/exdata\n"
                    "  --account-id HEX    account id for act.dat/rif (default 0)\n"
                    "  --rif-only          reuse the console's EXISTING act.dat and write\n"
                    "                      only the .rif (for already activated consoles;\n"
                    "                      never overwrites act.dat). Needs --idps.\n"
                    "  --force             allow regenerating an act.dat that already exists\n"
                    "                      (WARNING: breaks existing licenses on the console!)\n"
                    "  --write             actually write (default: dry run)\n"
                    "Without --idps, only the raw RAP is placed in /dev_hdd0/exdata.\n");
        return 1;
    }
    const std::string device = argv[1];
    std::string rap_path, content_id, idps_hex, account_hex;
    bool do_write = false, rif_only = false, force = false;
    for (int i = 3; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--write") do_write = true;
        else if (a == "--rif-only") rif_only = true;
        else if (a == "--force") force = true;
        else if (a == "--content-id" && i + 1 < argc) content_id = argv[++i];
        else if (a == "--idps" && i + 1 < argc) idps_hex = argv[++i];
        else if (a == "--account-id" && i + 1 < argc) account_hex = argv[++i];
        else if (rap_path.empty()) rap_path = a;
    }
    if (content_id.empty()) content_id = content_id_from_path(rap_path);
    const bool activate = !idps_hex.empty();

    std::printf("== ps3hdd_license %s ==\nDevice: %s\nRAP: %s\nContent ID: %s\n\n", do_write ? "(WRITE)" : "(DRY RUN)", device.c_str(), rap_path.c_str(), content_id.c_str());

    try {
        std::vector<std::byte> rap; // read
        if (FILE* f = std::fopen(rap_path.c_str(), "rb")) {
            std::uint8_t buf[64];
            std::size_t n = std::fread(buf, 1, sizeof(buf), f);
            std::fclose(f);
            for (std::size_t i = 0; i < n; ++i) rap.push_back(static_cast<std::byte>(buf[i]));
        } else { std::printf("cannot open RAP file\n"); return 1; }
        if (rap.size() != 16) { std::printf("RAP must be exactly 16 bytes (got %zu)\n", rap.size()); return 1; }

        const auto klic = license::rap_to_klicensee(rap);
        std::printf("Derived klicensee: ");
        for (auto b : klic) std::printf("%02x", std::to_integer<int>(b));
        std::printf("\n");
        if (content_id.size() < 36)
            std::printf("WARNING: content id looks short; the console expects the full " "XXNNNN-TITLEID_00-XXXXXXXXXXXXXXXX so it matches the game's NPDRM header.\n");

        const auto eid = parse_hex(argv[2]);
        if (eid.size() != 48) { std::printf("EID Root Key must be 48 bytes\n"); return 1; }

        auto raw = disk::open_raw_device(device, /*writable=*/do_write);
        auto src = std::make_shared<disk::physical_disk_source>(std::move(raw));
        std::printf("Disk: %s\n", src->description().c_str());
        auto m = app::open_gameos(src, {eid.data(), eid.size()});
        if (!m) { std::printf("Could not locate/mount the GameOS partition.\n"); return 1; }

        fs::ufs2_filesystem ufs(*m->decrypted, m->partition_sector);
        if (!ufs.mount()) { std::printf("mount failed\n"); return 1; }


        if (activate) { // activate licenes
            const auto idps = parse_hex(idps_hex);
            if (idps.size() != 16) { std::printf("IDPS must be 16 bytes\n"); return 1; }

            // --rif-only: reuse the consoles existing act.dat (real or fake) and write ONLY the .rif 
            // never overwrites act.dat, so existing licenses on an already activated console are preserved
            if (rif_only) {
                const std::string exdir = "home/00000001/exdata";
                const auto act_inode = ufs.resolve_path(exdir + "/act.dat");
                if (!act_inode) {
                    std::printf("No act.dat at /dev_hdd0/%s/act.dat.\n This console has no activation; run WITHOUT --rif-only (with --account-id)\n to create one, or activate the account first.\n", exdir.c_str());
                    return 1;
                }
                const auto actbytes = ufs.read_inode_data(*act_inode);
                if (actbytes.size() != 0x1038) {
                    std::printf("existing act.dat has size %zu (expected 0x1038)\n", actbytes.size());
                    return 1;
                }
                const auto rif = license::build_rif({idps.data(), idps.size()}, content_id, {klic.data(), klic.size()}, {actbytes.data(), actbytes.size()});
                if (license::rif_recover_klicensee({idps.data(), idps.size()}, {actbytes.data(), actbytes.size()}, {rif.data(), rif.size()}) != klic) {
                    std::printf("internal error: klicensee round trip vs existing act.dat failed\n");
                    return 1;
                }
                std::printf("Reusing existing act.dat (accountId bytes ");
                for (int i = 0; i < 8; ++i) std::printf("%02x", std::to_integer<int>(actbytes[0x08 + i]));
                std::printf(").\nTarget: /dev_hdd0/%s/%s.rif  (act.dat untouched)\n\n", exdir.c_str(), content_id.c_str());

                if (!do_write) {
                    std::printf("DRY RUN! Re-run with --write to write only the .rif.\n After writing: Rebuild Database on the console, then launch.\n");
                    return 0;
                }
                auto exinode = ufs.resolve_path_to_inode_number(exdir);
                if (!exinode) { std::printf("%s not found\n", exdir.c_str()); return 1; }
                fs::ufs2_writer writer(ufs, *m->decrypted);
                const std::string fname = content_id + ".rif";
                if (ufs.resolve_path(exdir + "/" + fname)) writer.delete_file(*exinode, fname);
                const std::vector<std::byte> v(rif.begin(), rif.end());
                writer.write_file(*exinode, fname, v);
                writer.update_superblock();

                fs::ufs2_filesystem check(*m->decrypted, m->partition_sector); check.mount();
                const auto ri = check.resolve_path(exdir + "/" + fname);
                if (!ri || check.read_inode_data(*ri) != v) { std::printf("VERIFY FAILED\n"); return 2; }
                std::printf("Wrote + verified %s (act.dat untouched)\n", fname.c_str());
                std::printf("Checking filesystem consistency ...\n");
                const auto rep = fs::check_consistency(check, *m->decrypted);
                std::printf("  cross_links=%lld out_of_range=%lld used_but_free=%lld\n", (long long)rep.cross_links, (long long)rep.out_of_range, (long long)rep.used_but_free);
                if (!rep.clean()) {
                    for (const auto& f : rep.findings) std::printf("  %s\n", f.c_str());
                    std::printf("\nFAILED - filesystem inconsistent. Restore the disk.\n");
                    return 3;
                }
                std::printf("\nSUCCESS - rif written against the existing act.dat. Rebuild Database, then launch.\n");
                return 0;
            }

            // match xai_plugin: 
            // the accountId is byte swapped before it lands in act.dat/rif
            // pass --account-id exactly as xai "Show accountID displays it
            // SWAP64
            auto bswap64 = [](std::uint64_t v) {
                std::uint64_t r = 0;
                for (int i = 0; i < 8; ++i) { r = (r << 8) | (v & 0xFF); v >>= 8; }
                return r;
            };
            const std::uint64_t account_id =
                account_hex.empty() ? 0 : bswap64(std::strtoull(account_hex.c_str(), nullptr, 16));

            const auto act = license::build_activation({idps.data(), idps.size()}, content_id, {klic.data(), klic.size()}, account_id);
            // selfcheck: 
            // run the console's inverse ladder before writing
            if (license::rif_recover_klicensee({idps.data(), idps.size()}, {act.act_dat.data(), act.act_dat.size()}, {act.rif.data(), act.rif.size()}) != klic) {
                std::printf("internal error: klicensee round trip failed\n");
                return 1;
            }

            // TODO
            // scan all user profiles and check for an act.dat there
            const std::string exdir = "home/00000001/exdata";

            if (ufs.resolve_path(exdir + "/act.dat") && !force) {
                std::printf("An act.dat already exists at /dev_hdd0/%s/act.dat.\n"
                            "Overwriting it would BREAK existing licenses on this console.\n"
                            "  - To add THIS game's license safely, use --rif-only.\n"
                            "  - To deliberately regenerate act.dat anyway, pass --force.\n",
                            exdir.c_str());
                return 1;
            }
            if (account_hex.empty()) {
                std::printf("NOTE: no --account-id given (account 0). On a console that has never had an\n"
                            "account, first set one in xai_plugin -> \"Create accountID\", then pass that\n"
                            "value as --account-id; account 0 alone yields BAD_ACT (0x80029514).\n\n");
            }

            std::printf("Target: /dev_hdd0/%s/act.dat\n        /dev_hdd0/%s/%s.rif\n"
                        "Account ID: %s (stored bytes %016llx)\n\n", exdir.c_str(), exdir.c_str(),
                        content_id.c_str(), account_hex.empty() ? "0 (empty)" : account_hex.c_str(),
                        static_cast<unsigned long long>(account_id));

            if (!do_write) {
                std::printf("DRY RUN. Re-run with --write to generate act.dat + rif.\n"
                            "After writing: Rebuild Database on the console, then launch. No other tools.\n");
                return 0;
            }

            auto userinode = ufs.resolve_path_to_inode_number("home/00000001");
            if (!userinode) { std::printf("home/00000001 not found on disk\n"); return 1; }
            fs::ufs2_writer writer(ufs, *m->decrypted);
            auto exinode = ufs.resolve_path_to_inode_number(exdir);
            if (!exinode) {
                std::printf("Creating /dev_hdd0/%s ...\n", exdir.c_str());
                writer.create_directory(*userinode, "exdata");
                fs::ufs2_filesystem re(*m->decrypted, m->partition_sector); re.mount();
                exinode = re.resolve_path_to_inode_number(exdir);
            }
            if (!exinode) { std::printf("failed to create exdata\n"); return 1; }

            auto write_one = [&](const std::string& name, std::span<const std::byte> data) {
                if (ufs.resolve_path(exdir + "/" + name)) writer.delete_file(*exinode, name);
                std::vector<std::byte> v(data.begin(), data.end());
                writer.write_file(*exinode, name, v);
            };
            write_one("act.dat", {act.act_dat.data(), act.act_dat.size()});
            write_one(content_id + ".rif", {act.rif.data(), act.rif.size()});
            writer.update_superblock();

            fs::ufs2_filesystem check(*m->decrypted, m->partition_sector); check.mount();
            const auto ai = check.resolve_path(exdir + "/act.dat");
            const auto ri = check.resolve_path(exdir + "/" + content_id + ".rif");
            const std::vector<std::byte> exp_act(act.act_dat.begin(), act.act_dat.end());
            const std::vector<std::byte> exp_rif(act.rif.begin(), act.rif.end());
            if (!ai || !ri || check.read_inode_data(*ai) != exp_act || check.read_inode_data(*ri) != exp_rif) {
                std::printf("VERIFY FAILED\n"); return 2;
            }
            std::printf("Wrote + verified act.dat and %s.rif\n", content_id.c_str());

            std::printf("Checking filesystem consistency ...\n");
            const auto rep = fs::check_consistency(check, *m->decrypted);
            std::printf("  cross_links=%lld out_of_range=%lld used_but_free=%lld\n", (long long)rep.cross_links, (long long)rep.out_of_range, (long long)rep.used_but_free);
            if (!rep.clean()) {
                for (const auto& f : rep.findings) std::printf("  %s\n", f.c_str());
                std::printf("\nFAILED! filesystem inconsistent. Restore the disk.\n");
                return 3;
            }
            std::printf("\nSUCCESS! activation written. Now Rebuild Database,\n then launch\n");
            return 0;
        }

        const std::string target = "exdata/" + content_id + ".rap";
        std::printf("Target: /dev_hdd0/%s\n\n", target.c_str());

        if (!do_write) {
            std::printf("DRY RUN. Re-run with --write to write the RAP.\n"
                        "After writing: on the console, ensure a user is activated (Apollo Save Tool\n"
                        "offline activation if needed), then run a RAP installer (Apollo / reActPSN /\n"
                        "xai_plugin) to convert exdata/*.rap into home/<user>/exdata/*.rif.\n");
            return 0;
        }

        fs::ufs2_writer writer(ufs, *m->decrypted);
        // ensure /dev_hdd0/exdata exists
        auto exdata = ufs.resolve_path_to_inode_number("exdata");
        if (!exdata) {
            std::printf("Creating /dev_hdd0/exdata ...\n");
            writer.create_directory(fs::ufs2_filesystem::root_inode, "exdata");
            fs::ufs2_filesystem re(*m->decrypted, m->partition_sector);
            re.mount();
            exdata = re.resolve_path_to_inode_number("exdata");
        }
        if (!exdata) { std::printf("failed to create exdata\n"); return 1; }

        const std::string fname = content_id + ".rap";
        if (ufs.resolve_path("exdata/" + fname)) {
            std::printf("Replacing existing %s ...\n", fname.c_str());
            writer.delete_file(*exdata, fname);
        }
        writer.write_file(*exdata, fname, rap);
        writer.update_superblock();
        std::printf("Wrote /dev_hdd0/%s\n", target.c_str());

        fs::ufs2_filesystem check(*m->decrypted, m->partition_sector);
        check.mount();
        const auto in = check.resolve_path(target);
        if (!in || check.read_inode_data(*in) != rap) { std::printf("VERIFY FAILED\n"); return 2; }
        std::printf("Verified RAP contents on disk.\n");

        std::printf("Checking filesystem consistency ...\n"); //redundant
        const auto rep = fs::check_consistency(check, *m->decrypted); 
        std::printf("  cross_links=%lld out_of_range=%lld used_but_free=%lld\n", (long long)rep.cross_links, (long long)rep.out_of_range, (long long)rep.used_but_free);
        if (!rep.clean()) {
            for (const auto& f : rep.findings) std::printf("  %s\n", f.c_str());
            std::printf("\nFAILED - filesystem inconsistent. Restore the disk and report this.\n");
            return 3;
        }
        std::printf("\nSUCCESS! Next, on the console: activate a user, then run xai_plugin to turn exdata/*.rap\n into a console bound .rif, then Rebuild Database.\n");
        return 0;
    } catch (const std::exception& ex) {
        std::printf("\nERROR: %s\n", ex.what());
        return 1;
    }
}