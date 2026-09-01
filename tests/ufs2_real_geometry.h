#pragma once

#include "sparse_disk_source.h"

#include <ps3hdd_crypto/be_io.h>
#include <ps3hdd_fs/ufs2_filesystem.h>
#include <ps3hdd_fs/ufs2_writer.h>

#include <cstring>
#include <vector>

namespace trg {

using ps3hdd::write_be_u32;
using ps3hdd::write_be_u64;
using ps3hdd::fs::cylinder_group;
using ps3hdd::fs::superblock;

inline constexpr int kFrag = 4096;
inline constexpr int kBlock = 16384;
inline constexpr int kFpb = kBlock / kFrag; // 4
inline constexpr int kNcg = 3220;
inline constexpr int kFpg = 73900;
inline constexpr int kIpg = 36992;
inline constexpr int kCblkno = 24;
inline constexpr int kIblkno = 28; // 2340 - (36992*256/4096)
inline constexpr int kDblkno = 2340;
inline constexpr int kCgSize = 16384;
inline constexpr std::int64_t kTotalFrags = 237899188;

inline constexpr int kIusedoff = 168;
inline constexpr int kFreeoff = kIusedoff + 4624;        // 4792
inline constexpr int kClustersumoff = kFreeoff + 9238;   // 14030
inline constexpr int kClusteroff = kClustersumoff + 40;  // room for (contigsum+1) int32
inline constexpr int kNclusterblks = kFpg / kFpb;        // 18475 -> 2310 B, ends 16364 < 16384
inline constexpr int kInitInited = 64;

inline void set_bit(std::vector<std::byte>& d, std::size_t base, int idx) {
    d[base + idx / 8] |= static_cast<std::byte>(1 << (idx % 8));
}

inline std::uint64_t cg_header_offset(int cg) {
    return static_cast<std::uint64_t>(static_cast<std::int64_t>(cg) * kFpg + kCblkno) * kFrag;
}

inline void write_superblock(th::sparse_disk_source& d) {
    std::vector<std::byte> b(8192, std::byte{0});
    write_be_u32(b.data() + 0x55C, superblock::magic_value);
    write_be_u32(b.data() + 0x30, kBlock);
    write_be_u32(b.data() + 0x34, kFrag);
    write_be_u32(b.data() + 0xBC, kFpg);
    write_be_u32(b.data() + 0xB8, kIpg);
    write_be_u32(b.data() + 0x10, kIblkno);
    write_be_u32(b.data() + 0x0C, kCblkno);
    write_be_u32(b.data() + 0x14, kDblkno);
    write_be_u32(b.data() + 0xA0, kCgSize);
    write_be_u32(b.data() + 0x2C, kNcg);
    write_be_u32(b.data() + 0x38, kFpb);
    write_be_u32(b.data() + 0x54, 12);                 // fs_fshift  (log2 4096)
    write_be_u32(b.data() + 0x5C, kBlock / 8);         // fs_maxbpg
    write_be_u32(b.data() + 0x60, 2);                  // fs_fragshift (log2 4)
    write_be_u32(b.data() + 0x64, 3);                  // fs_fsbtodb (4096/512)
    write_be_u32(b.data() + 0x74, kBlock / 8);         // fs_nindir
    write_be_u32(b.data() + 0x9C, kNcg * 16);          // fs_cssize
    write_be_u64(b.data() + 0x3E8, 65536);             // fs_sblockloc
    write_be_u64(b.data() + 0x438, kTotalFrags);       // fs_size
    write_be_u64(b.data() + 0x440, kTotalFrags - kDblkno); // fs_dsize
    write_be_u64(b.data() + 0x448, kDblkno);           // fs_csaddr (== cgdmin(fs,0))
    write_be_u32(b.data() + 0x4AC, kBlock);            // fs_avgfilesize
    write_be_u32(b.data() + 0x4B0, 64);                // fs_avgfpdir
    write_be_u32(b.data() + 0x524, 8);                 // fs_contigsumsize
    d.write_bytes(65536, {b.data(), b.size()});
}

inline int cs_frags() {
    const int bytes = kNcg * 16;
    const int frags = (bytes + kFrag - 1) / kFrag;
    return ((frags + kFpb - 1) / kFpb) * kFpb; // round up to whole blocks
}

inline void write_cg_header(th::sparse_disk_source& d, int cg) {
    std::vector<std::byte> h(kCgSize, std::byte{0});
    write_be_u32(h.data() + 0x04, cylinder_group::magic_value);
    write_be_u32(h.data() + 0x0C, static_cast<std::uint32_t>(cg));
    write_be_u32(h.data() + 0x14, kFpg);
    const int first_free = (cg == 0) ? kDblkno + cs_frags() : kDblkno;
    write_be_u32(h.data() + 0x1C, static_cast<std::uint32_t>((kFpg - first_free) / kFpb));
    write_be_u32(h.data() + 0x20, kIpg);
    write_be_u32(h.data() + 0x24, 0);
    write_be_u32(h.data() + 0x5C, kIusedoff);
    write_be_u32(h.data() + 0x60, kFreeoff);
    write_be_u32(h.data() + 0x68, kClustersumoff);
    write_be_u32(h.data() + 0x6C, kClusteroff);
    write_be_u32(h.data() + 0x70, kNclusterblks);
    write_be_u32(h.data() + 0x78, kInitInited);
    for (int f = first_free; f < kFpg; ++f) set_bit(h, kFreeoff, f);
    for (int bl = first_free / kFpb; bl < kNclusterblks; ++bl) set_bit(h, kClusteroff, bl);
    // one contiguous free run, longer than contigsumsize, so it lands in sums[contigsum]
    write_be_u32(h.data() + kClustersumoff + 4 * 8, 1);
    d.write_bytes(cg_header_offset(cg), {h.data(), h.size()});
}

inline void write_cs_array_and_totals(th::sparse_disk_source& d) {
    std::vector<std::byte> cs(static_cast<std::size_t>(kNcg) * 16, std::byte{0});
    std::uint64_t t_nbfree = 0, t_nifree = 0;
    for (int i = 0; i < kNcg; ++i) {
        const int first_free = (i == 0) ? kDblkno + cs_frags() : kDblkno;
        const std::uint32_t nbfree = static_cast<std::uint32_t>((kFpg - first_free) / kFpb);
        write_be_u32(cs.data() + static_cast<std::size_t>(i) * 16 + 0, 0);
        write_be_u32(cs.data() + static_cast<std::size_t>(i) * 16 + 4, nbfree);
        write_be_u32(cs.data() + static_cast<std::size_t>(i) * 16 + 8, kIpg);
        write_be_u32(cs.data() + static_cast<std::size_t>(i) * 16 + 12, 0);
        t_nbfree += nbfree;
        t_nifree += kIpg;
    }
    d.write_bytes(static_cast<std::uint64_t>(kDblkno) * kFrag, {cs.data(), cs.size()});

    auto sb = d.read_bytes(65536, 8192);
    write_be_u64(sb.data() + 0x3F0, 0);         // cs_ndir
    write_be_u64(sb.data() + 0x3F8, t_nbfree);  // cs_nbfree
    write_be_u64(sb.data() + 0x400, t_nifree);  // cs_nifree
    write_be_u64(sb.data() + 0x408, 0);         // cs_nffree
    d.write_bytes(65536, {sb.data(), sb.size()});
}

inline th::sparse_disk_source build_real_geometry_image() {
    th::sparse_disk_source d(static_cast<std::uint64_t>(kTotalFrags) * kFrag);
    write_superblock(d);
    for (int i = 0; i < kNcg; ++i) write_cg_header(d, i);
    write_cs_array_and_totals(d);
    return d;
}

inline void bootstrap_root(ps3hdd::fs::ufs2_writer& w) {
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

} // namespace trg