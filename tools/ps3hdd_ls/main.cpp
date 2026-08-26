#include "../common.h"

#include <ps3hdd_app/gameos.h>
#include <ps3hdd_disk/disk_source.h>
#include <ps3hdd_disk/physical_disk_source.h>
#include <ps3hdd_disk/raw_device.h>
#include <ps3hdd_fs/ufs2_filesystem.h>

#include <cstdint>
#include <cstdio>
#include <exception>
#include <memory>
#include <string>
#include <vector>

using namespace ps3hdd;
using ps3hdd::tools::parse_hex;
using ps3hdd::tools::hexdump;

namespace {

const char* type_name(fs::dirent_type t) {
    switch (t) {
        case fs::dirent_type::directory: return "DIR ";
        case fs::dirent_type::regular_file: return "file";
        case fs::dirent_type::symbolic_link: return "link";
        default: return "?   ";
    }
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::printf("usage: ps3hdd_ls <device> <eid_key_hex> [path] [--extract <outfile>]\n");
        return 1;
    }
    const std::string device = argv[1];
    std::string path, extract;
    for (int i = 3; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--extract" && i + 1 < argc) extract = argv[++i];
        else if (path.empty()) path = a;
    }

    try {
        const auto eid = parse_hex(argv[2]);
        if (eid.size() != 48) { std::printf("EID Root Key must be 48 bytes\n"); return 1; }

        auto raw = disk::open_raw_device(device, /*writable=*/false);
        auto src = std::make_shared<disk::physical_disk_source>(std::move(raw));
        auto m = app::open_gameos(src, {eid.data(), eid.size()});
        if (!m) { std::printf("Could not locate/mount the GameOS partition.\n"); return 1; }
        fs::ufs2_filesystem ufs(*m->decrypted, m->partition_sector);
        if (!ufs.mount()) { std::printf("mount failed\n"); return 1; }

        const auto node = path.empty() ? ufs.read_inode(fs::ufs2_filesystem::root_inode) : [&] {
            auto in = ufs.resolve_path(path);
            if (!in) throw std::runtime_error("path not found: " + path);
            return *in;
        }();

        if (node.is_directory()) {
            std::printf("%s/  (%lld bytes)\n", path.empty() ? "" : path.c_str(), (long long)node.size);
            for (const auto& e : ufs.read_directory(node)) {
                if (e.name == "." || e.name == "..") continue;
                const auto ci = ufs.read_inode(e.inode_number);
                std::printf("  %s  %12lld  %s\n", type_name(e.type), (long long)ci.size, e.name.c_str());
            }
        } else {
            std::printf("%s  (%lld bytes)\n", path.c_str(), (long long)node.size);
            auto data = ufs.read_inode_data(node);
            hexdump(data, 256);
            if (!extract.empty()) {
                if (FILE* f = std::fopen(extract.c_str(), "wb")) {
                    std::fwrite(data.data(), 1, data.size(), f);
                    std::fclose(f);
                    std::printf("\nExtracted %lld bytes -> %s\n", (long long)data.size(), extract.c_str());
                } else {
                    std::printf("could not open %s for writing\n", extract.c_str());
                }
            }
        }
        return 0;
    } catch (const std::exception& ex) {
        std::printf("ERROR: %s\n", ex.what());
        return 1;
    }
}