#include "../common.h"

#include <ps3hdd_app/gameos.h>
#include <ps3hdd_disk/physical_disk_source.h>
#include <ps3hdd_disk/raw_device.h>
#include <ps3hdd_fs/ufs2_checker.h>
#include <ps3hdd_fs/ufs2_filesystem.h>
#include <ps3hdd_fs/ufs2_writer.h>

#include <cstdio>
#include <exception>
#include <filesystem>
#include <optional>
#include <memory>
#include <string>
#include <vector>

using namespace ps3hdd;
using ps3hdd::tools::parse_hex;
using ps3hdd::tools::read_file;

int main(int argc, char** argv) {
    std::vector<std::string> args(argv + 1, argv + argc);
    bool do_write = false;
    std::vector<std::string> pos;
    for (auto& a : args) { if (a == "--write") do_write = true; else pos.push_back(a); }
    if (pos.size() < 4) {
        std::fprintf(stderr, "usage: %s <device> <eid_key_hex> <gameos_dir> <file...> [--write]\n", argv[0]);
        return 2;
    }
    const std::string device = pos[0];
    const auto eid = parse_hex(pos[1]);
    const std::string gdir = pos[2];
    std::vector<std::string> files(pos.begin() + 3, pos.end());
    if (eid.size() != 48) { std::fprintf(stderr, "EID key must be 48 bytes (got %zu)\n", eid.size()); return 2; }

    try {
        auto raw = disk::open_raw_device(device, /*writable=*/do_write);
        auto src = std::make_shared<disk::physical_disk_source>(std::move(raw));
        auto m = app::open_gameos(src, {eid.data(), eid.size()});
        if (!m) { std::fprintf(stderr, "could not mount GameOS on %s\n", device.c_str()); return 1; }
        std::printf("GameOS mounted (%s) @ sector 0x%llx\n", m->cipher.c_str(), static_cast<unsigned long long>(m->partition_sector));

        auto mount_fs = [&] {
            auto f = std::make_unique<fs::ufs2_filesystem>(*m->decrypted, m->partition_sector);
            if (!f->mount()) throw std::runtime_error("mount failed");
            return f;
        };
        auto ufs = mount_fs();

        //ensure the target directory exists (mkdir -p), creating missing components.
        std::optional<std::uint64_t> dir = ufs->resolve_path_to_inode_number(gdir);
        if (!dir) {
            if (!do_write) { std::printf("directory %s does not exist; --write will create it.\n", gdir.c_str()); }
            else {
                std::uint64_t parent = fs::ufs2_filesystem::root_inode;
                std::string path;
                std::string seg;
                std::vector<std::string> parts;
                for (char c : gdir) { if (c == '/') { if (!seg.empty()) parts.push_back(seg); seg.clear(); } else seg.push_back(c); }
                if (!seg.empty()) parts.push_back(seg);
                for (const auto& part : parts) {
                    path = path.empty() ? part : path + "/" + part;
                    auto ino = ufs->resolve_path_to_inode_number(path);
                    if (!ino) {
                        fs::ufs2_writer w(*ufs, *m->decrypted);
                        w.create_directory(parent, part);
                        w.update_superblock();
                        ufs = mount_fs();  // refresh so the new dir resolves
                        ino = ufs->resolve_path_to_inode_number(path);
                        if (!ino) { std::fprintf(stderr, "failed to create %s\n", path.c_str()); return 1; }
                        std::printf("  created dir %s\n", path.c_str());
                    }
                    parent = *ino;
                }
                dir = parent;
            }
        }

        for (const auto& fp : files) {
            const auto data = read_file(fp);
            const std::string name = std::filesystem::path(fp).filename().string();
            std::printf("  %-28s -> %s/%s  (%zu bytes)%s\n", fp.c_str(), gdir.c_str(), name.c_str(), data.size(), do_write ? "" : "   [dry-run]");
        }
        if (!do_write) { std::printf("dry run: pass --write to apply.\n"); return 0; }

        fs::ufs2_writer writer(*ufs, *m->decrypted);
        for (const auto& fp : files) {
            const auto data = read_file(fp);
            const std::string name = std::filesystem::path(fp).filename().string();
            if (ufs->resolve_path(gdir + "/" + name)) writer.delete_file(*dir, name);
            writer.write_file(*dir, name, data);
        }
        writer.update_superblock();

        // remount fresh and verify global consistency before declaring success.
        fs::ufs2_filesystem chk(*m->decrypted, m->partition_sector); chk.mount();
        const auto rep = fs::check_consistency(chk, *m->decrypted);
        std::printf("consistency: %s -> %s\n", rep.summary_line().c_str(), rep.clean() ? "CLEAN" : (rep.safe_to_write() ? "OK (orphans present)" : "INCONSISTENT"));
        if (!rep.safe_to_write()) { std::fprintf(stderr, "REFUSING: filesystem inconsistent after write!\n"); return 1; }
        std::printf("wrote %zu file(s) to %s. OK.\n", files.size(), gdir.c_str());
        return 0;
    } catch (const std::exception& ex) {
        std::fprintf(stderr, "error: %s\n", ex.what());
        return 1;
    }
}