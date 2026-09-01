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

namespace {

void show_placement(fs::ufs2_filesystem& ufs, std::uint64_t ino, const std::string& name, int depth) {
    if (depth > 8) return;
    const auto ipg = ufs.sb().inodes_per_group;
    fs::inode in;
    try { in = ufs.read_inode(ino); } catch (const std::exception&) { return; }
    std::printf("  %*s%-30s inode %-9llu cg %lld%s\n", depth * 2, "", name.c_str(), (unsigned long long)ino, (long long)(ipg > 0 ? ino / ipg : 0),in.is_directory() ? "" : "  (file)");
    if (!in.is_directory()) return;
    std::vector<fs::directory_entry> ents;
    try { ents = ufs.read_directory(in); } catch (const std::exception&) { return; }
    for (const auto& e : ents) {
        if (e.name == "." || e.name == "..") continue;
        show_placement(ufs, e.inode_number, e.name, depth + 1);
    }
}

bool dump_metadata(fs::ufs2_filesystem& ufs, disk::disk_source& disk, const std::string& path) {
    const auto& sb = ufs.sb();
    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    auto put = [&](const std::vector<std::byte>& v) { std::fwrite(v.data(), 1, v.size(), f); };

    const auto sbd = disk.read_bytes(ufs.partition_offset_bytes() + 65536, 8192);
    put(sbd);
    std::size_t cs_bytes = 0;
    if (sb.cs_address > 0 && sb.cylinder_groups > 0) {
        cs_bytes = static_cast<std::size_t>(sb.cylinder_groups) * 16;
        put(disk.read_bytes(ufs.partition_offset_bytes() + static_cast<std::uint64_t>(sb.cs_address) * sb.fragment_size, cs_bytes));
    }
    for (int cg = 0; cg < sb.cylinder_groups; ++cg) {
        const std::uint64_t off = ufs.partition_offset_bytes() +
            static_cast<std::uint64_t>(cg) * sb.frags_per_group * sb.fragment_size +
            static_cast<std::uint64_t>(sb.cg_block_offset) * sb.fragment_size;
        put(disk.read_bytes(off, static_cast<std::size_t>(sb.cg_size)));
    }
    std::fclose(f);
    std::printf("wrote %s\n  layout: superblock 8192 B, cs array %zu B, then %d cg headers of %d B\n  total %.1f MB\n", path.c_str(), cs_bytes, sb.cylinder_groups, sb.cg_size, (8192.0 + cs_bytes + double(sb.cylinder_groups) * sb.cg_size) / (1024 * 1024));
    return true;
}

std::uint32_t be32(const std::byte* p) {
    return (std::to_integer<std::uint32_t>(p[0]) << 24) | (std::to_integer<std::uint32_t>(p[1]) << 16) | (std::to_integer<std::uint32_t>(p[2]) << 8) | std::to_integer<std::uint32_t>(p[3]);
}
std::uint64_t be64(const std::byte* p) {
    std::uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v = (v << 8) | std::to_integer<std::uint64_t>(p[i]);
    return v;
}

struct findings {
    std::uint64_t files = 0, dirs = 0, symlinks = 0, other = 0;
    std::uint64_t sparse_files = 0, holes = 0;
    std::uint64_t extattr_inodes = 0;
    std::uint64_t inline_symlinks = 0, block_symlinks = 0;
    std::uint64_t unreadable = 0;
    std::vector<std::string> examples;

    void example(const std::string& s) {
        if (examples.size() < 24) examples.push_back(s);
    }
};

void walk(fs::ufs2_filesystem& ufs, std::uint64_t ino, const std::string& path, findings& f, int depth) {
    if (depth > 64) return;
    fs::inode in;
    try {
        in = ufs.read_inode(ino);
    } catch (const std::exception&) {
        ++f.unreadable;
        return;
    }

    try {
        const auto raw = ufs.read_inode_raw(ino);
        if (raw.size() >= 0x70) {
            const std::uint32_t extsize = be32(raw.data() + 0x5C);
            const std::uint64_t extb0 = be64(raw.data() + 0x60);
            const std::uint64_t extb1 = be64(raw.data() + 0x68);
            if (extsize != 0 || extb0 != 0 || extb1 != 0) {
                ++f.extattr_inodes;
                char buf[400];
                std::snprintf(buf, sizeof buf, "EXTATTR  %s  di_extsize=%u di_extb=[%llu,%llu]", path.c_str(), extsize, (unsigned long long)extb0, (unsigned long long)extb1);
                f.example(buf);
            }
        }
    } catch (const std::exception&) {
    }

    if (in.type == fs::file_type::directory) {
        ++f.dirs;
        std::vector<fs::directory_entry> entries;
        try {
            entries = ufs.read_directory(in);
        } catch (const std::exception&) {
            ++f.unreadable;
            return;
        }
        for (const auto& e : entries) {
            if (e.name == "." || e.name == "..") continue;
            walk(ufs, e.inode_number, path + "/" + e.name, f, depth + 1);
        }
        return;
    }

    if (in.type == fs::file_type::symbolic_link) {
        ++f.symlinks;
        const bool inlined = in.size > 0 && in.size < 120;
        if (inlined) ++f.inline_symlinks; else ++f.block_symlinks;
        std::string target;
        try {
            const auto raw = ufs.read_inode_raw(ino);
            if (inlined && raw.size() >= 0x70 + static_cast<std::size_t>(in.size))
                target.assign(reinterpret_cast<const char*>(raw.data() + 0x70), static_cast<std::size_t>(in.size));
        } catch (const std::exception&) {
        }
        char buf[500];
        std::snprintf(buf, sizeof buf, "SYMLINK  %s  size=%lld %s -> %s", path.c_str(), (long long)in.size, inlined ? "(inline)" : "(block)", target.c_str());
        f.example(buf);
        return;
    }

    if (in.type != fs::file_type::regular_file) { ++f.other; return; }
    ++f.files;
    if (in.size <= 0) return;

    try {
        const auto ptrs = ufs.block_pointers_with_holes(in);
        std::uint64_t local_holes = 0;
        for (std::size_t i = 0; i < ptrs.size(); ++i)
            if (ptrs[i] == 0) ++local_holes;
        if (local_holes) {
            ++f.sparse_files;
            f.holes += local_holes;
            char buf[400];
            std::snprintf(buf, sizeof buf, "SPARSE   %s  size=%lld blocks=%zu holes=%llu", path.c_str(), (long long)in.size, ptrs.size(), (unsigned long long)local_holes);
            f.example(buf);
        }
    } catch (const std::exception&) {
        ++f.unreadable;
    }
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::printf("usage: ps3hdd_scan <device|image> <eid_key_hex>\nREAD-ONLY. Audits the GameOS filesystem for sparse files, extended\n attributes and symlinks, the shapes the reader does not yet handle.\n");
        return 1;
    }
    const std::string device = argv[1];
    std::string placement_path, dump_path;
    for (int i = 3; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--placement" && i + 1 < argc) placement_path = argv[++i];
        else if (a == "--dump-meta" && i + 1 < argc) dump_path = argv[++i];
    }
    try {
        const auto eid = parse_hex(argv[2]);
        if (eid.size() != 48) { std::printf("EID Root Key must be 48 bytes\n"); return 1; }

        auto raw = disk::open_raw_device(device, /*writable=*/false);
        auto src = std::make_shared<disk::physical_disk_source>(std::move(raw));
        std::printf("Disk: %s  (opened READ-ONLY)\n", src->description().c_str());

        auto m = app::open_gameos(src, {eid.data(), eid.size()});
        if (!m) { std::printf("Could not locate/mount the GameOS partition.\n"); return 1; }

        fs::ufs2_filesystem ufs(*m->decrypted, m->partition_sector);
        if (!ufs.mount()) { std::printf("mount failed\n"); return 1; }
        const auto& sb = ufs.sb();
        std::printf("Mounted: bsize=%lld fsize=%lld ncg=%d\n", (long long)sb.block_size, (long long)sb.fragment_size, sb.cylinder_groups);
        std::printf("Superblock extras: frag=%d maxbpg=%d nindir=%d avgfilesize=%d avgfpdir=%d csaddr=%lld\n\n", sb.frag, sb.max_blocks_per_group, sb.indirect_per_block, sb.avg_file_size, sb.avg_files_per_dir, (long long)sb.cs_address);

        if (!dump_path.empty()) {
            if (!dump_metadata(ufs, *m->decrypted, dump_path)) {
                std::printf("could not write %s\n", dump_path.c_str());
                return 1;
            }
            if (placement_path.empty()) return 0;
        }
        if (!placement_path.empty()) {
            const auto ino = ufs.resolve_path_to_inode_number(placement_path);
            if (!ino) { std::printf("path not found: %s\n", placement_path.c_str()); return 1; }
            std::printf("== PLACEMENT: %s ==\n", placement_path.c_str());
            show_placement(ufs, *ino, placement_path, 0);
            return 0;
        }

        std::printf("== WALKING TREE (read-only) ==\n");
        findings f;
        walk(ufs, fs::ufs2_filesystem::root_inode, "", f, 0);

        std::printf("\n== COUNTS ==\n");
        std::printf("  files=%llu dirs=%llu symlinks=%llu other=%llu unreadable=%llu\n", (unsigned long long)f.files, (unsigned long long)f.dirs, (unsigned long long)f.symlinks, (unsigned long long)f.other, (unsigned long long)f.unreadable);
        std::printf("\n== GAP 1: SPARSE FILES (silent misread today) ==\n");
        std::printf("  sparse files: %llu   total holes: %llu\n", (unsigned long long)f.sparse_files, (unsigned long long)f.holes);
        std::printf("\n== GAP 2: EXTENDED ATTRIBUTES (dropped on copy, leaked on delete) ==\n");
        std::printf("  inodes with extattrs: %llu\n", (unsigned long long)f.extattr_inodes);
        std::printf("\n== GAP 3: SYMLINKS (unhandled) ==\n");
        std::printf("  symlinks: %llu  (inline=%llu block=%llu)\n", (unsigned long long)f.symlinks, (unsigned long long)f.inline_symlinks, (unsigned long long)f.block_symlinks);

        if (!f.examples.empty()) {
            std::printf("\n== EXAMPLES (first %zu) ==\n", f.examples.size());
            for (const auto& e : f.examples) std::printf("  %s\n", e.c_str());
        }

        const bool any = f.sparse_files || f.extattr_inodes || f.symlinks;
        std::printf("\n== RESULT ==\n");
        std::printf(any ? "  AFFECTED - this drive contains shapes our reader mishandles (see above).\n" : "  CLEAR - none of the three gaps occur on this drive.\n");
        return 0;
    } catch (const std::exception& ex) {
        std::printf("\nERROR: %s\n", ex.what());
        return 1;
    }
}