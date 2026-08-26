#pragma once

#include "memory_disk_source.h"

#include <ps3hdd_crypto/be_io.h>
#include <ps3hdd_fs/ufs2_filesystem.h>
#include <ps3hdd_fs/ufs2_writer.h>

#include <cstring>
#include <vector>

namespace tuf {

using ps3hdd::write_be_u16;
using ps3hdd::write_be_u32;
using ps3hdd::fs::cylinder_group;
using ps3hdd::fs::superblock;
using ps3hdd::fs::ufs2_writer;

// 16 KB blocks
// 2 KB fragments 
//(8 frags/block)
inline constexpr int kFrag = 2048;
inline constexpr int kBlock = 16384;
inline constexpr int kFpb = kBlock / kFrag; // 8
inline constexpr int kIpg = 256;
inline constexpr int kFpg = 4096;
inline constexpr int kCblkno = 8;
inline constexpr int kDblkno = 80; // after the 32 frag inode table (iblkno 48)
inline constexpr int kIblkno = 48;
inline constexpr int kCgSize = 16384;
inline constexpr int kIusedoff = 168;
inline constexpr int kFreeoff = 208;
inline constexpr int kClusteroff = 720;
inline constexpr int kClustersumoff = 784;
inline constexpr int kNclusterblks = kFpg / kFpb;
inline constexpr int kInitInited = 64;

inline void set_bit(std::vector<std::byte>& d, std::size_t base, int idx) {
    d[base + idx / 8] |= static_cast<std::byte>(1 << (idx % 8));
}

inline void write_superblock(std::vector<std::byte>& img, int ncg) {
    std::byte* b = img.data() + 65536;
    write_be_u32(b + 0x55C, superblock::magic_value);
    write_be_u32(b + 0x30, kBlock);
    write_be_u32(b + 0x34, kFrag);
    write_be_u32(b + 0xBC, kFpg);
    write_be_u32(b + 0xB8, kIpg);
    write_be_u32(b + 0x10, kIblkno);
    write_be_u32(b + 0x0C, kCblkno);
    write_be_u32(b + 0x14, kDblkno);
    write_be_u32(b + 0xA0, kCgSize);
    write_be_u32(b + 0x2C, static_cast<std::uint32_t>(ncg));
}

inline std::uint64_t cg_header_offset(int cg) {
    return static_cast<std::uint64_t>(static_cast<std::int64_t>(cg) * kFpg + kCblkno) * kFrag;
}

inline void write_cg_header(std::vector<std::byte>& img, int cg) {
    const std::uint64_t base = cg_header_offset(cg);
    std::byte* h = img.data() + base;
    write_be_u32(h + 0x04, cylinder_group::magic_value);
    write_be_u32(h + 0x0C, static_cast<std::uint32_t>(cg));
    write_be_u32(h + 0x14, kFpg);
    write_be_u32(h + 0x1C, static_cast<std::uint32_t>((kFpg - kDblkno) / kFpb));
    write_be_u32(h + 0x20, kIpg);
    write_be_u32(h + 0x24, 0);
    write_be_u32(h + 0x5C, kIusedoff);
    write_be_u32(h + 0x60, kFreeoff);
    write_be_u32(h + 0x68, kClustersumoff);
    write_be_u32(h + 0x6C, kClusteroff);
    write_be_u32(h + 0x70, kNclusterblks);
    write_be_u32(h + 0x78, kInitInited);

    std::vector<std::byte> raw(img.begin() + base, img.begin() + base + kCgSize);
    for (int f = kDblkno; f < kFpg; ++f) set_bit(raw, kFreeoff, f);
    for (int bl = kDblkno / kFpb; bl < kNclusterblks; ++bl) set_bit(raw, kClusteroff, bl);
    std::memcpy(img.data() + base, raw.data(), raw.size());
}

inline th::memory_disk_source build_cg_image() {
    th::memory_disk_source disk(512 * 1024);
    write_superblock(disk.store(), 1);
    write_cg_header(disk.store(), 0);
    return disk;
}

inline th::memory_disk_source build_two_cg_image() {
    const std::size_t bytes = static_cast<std::size_t>(2) * kFpg * kFrag;
    th::memory_disk_source disk(bytes);
    write_superblock(disk.store(), 2);
    write_cg_header(disk.store(), 0);
    write_cg_header(disk.store(), 1);
    return disk;
}

inline void bootstrap_root(ufs2_writer& w) {
    auto& cg = w.read_cylinder_group(0);
    w.mark_inode_used(cg, 0);
    w.mark_inode_used(cg, 1);
    w.mark_inode_used(cg, 2);
    const std::int64_t frag = w.find_free_fragments(cg, kFpb);
    auto block = w.build_empty_directory_block(2, 2);
    w.write_data_block(frag, block);
    w.mark_fragments_used(cg, static_cast<int>(frag), kFpb);
    auto inode = w.build_directory_inode(frag, /*nlink=*/2);
    w.write_inode(2, inode);
    w.write_cylinder_group(cg);
    w.update_superblock();
}

inline std::vector<std::byte> pattern(std::size_t n, std::uint8_t seed) {
    std::vector<std::byte> v(n);
    for (std::size_t i = 0; i < n; ++i)
        v[i] = static_cast<std::byte>((i * 31 + seed) & 0xff);
    return v;
}

} // namespace tuf