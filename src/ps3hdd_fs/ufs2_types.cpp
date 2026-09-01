#include "ufs2_types.h"

#include <ps3hdd_crypto/be_io.h>

#include <vector>

namespace ps3hdd::fs {

using ps3hdd::read_be_u16;
using ps3hdd::read_be_u32;
using ps3hdd::read_be_u64;

namespace {
std::int32_t read_be_i32(std::span<const std::byte> b, std::size_t off) {
    return static_cast<std::int32_t>(read_be_u32(b, off));
}
std::int64_t read_be_i64(std::span<const std::byte> b, std::size_t off) {
    return static_cast<std::int64_t>(read_be_u64(b, off));
}
std::string read_fixed_cstr(std::span<const std::byte> b, std::size_t off, std::size_t max) {
    std::string s;
    for (std::size_t i = 0; i < max && off + i < b.size(); ++i) {
        char c = std::to_integer<char>(b[off + i]);
        if (c == '\0') break;
        s.push_back(c);
    }
    return s;
}
} // namespace

file_type file_type_from_mode(std::uint16_t mode) {
    switch (mode & 0xF000) {
        case 0x4000: return file_type::directory;
        case 0x8000: return file_type::regular_file;
        case 0xA000: return file_type::symbolic_link;
        case 0x6000: return file_type::block_device;
        case 0x2000: return file_type::char_device;
        case 0x1000: return file_type::fifo;
        case 0xC000: return file_type::socket;
        default:     return file_type::unknown;
    }
}

superblock superblock::parse(std::span<const std::byte> data) {
    superblock sb;
    if (data.size() > magic_offset + 4)
        sb.magic = read_be_u32(data, magic_offset);
    if (!sb.valid()) return sb;

    sb.block_size = read_be_i32(data, 0x30);          // fs_bsize
    sb.fragment_size = read_be_i32(data, 0x34);       // fs_fsize
    sb.frags_per_group = read_be_i32(data, 0xBC);     // fs_fpg
    sb.inodes_per_group = read_be_i32(data, 0xB8);    // fs_ipg
    sb.inode_block_offset = read_be_i32(data, 0x10);  // fs_iblkno
    sb.cg_block_offset = read_be_i32(data, 0x0C);     // fs_cblkno
    sb.data_block_offset = read_be_i32(data, 0x14);   // fs_dblkno
    sb.cg_size = read_be_i32(data, 0xA0);             // fs_cgsize
    sb.total_fragments = read_be_i64(data, 0x438);    // fs_size
    sb.total_data_fragments = read_be_i64(data, 0x440); // fs_dsize
    sb.cylinder_groups = read_be_i32(data, 0x2C);     // fs_ncg

    // fs_cstotal (five int64 fields) at 0x3F0.
    sb.directories = read_be_i64(data, 0x3F0);        // cs_ndir
    sb.free_blocks = read_be_i64(data, 0x3F8);        // cs_nbfree
    sb.free_inodes = read_be_i64(data, 0x400);        // cs_nifree
    sb.free_fragments = read_be_i64(data, 0x408);     // cs_nffree

    sb.frag = read_be_i32(data, 0x38);                 // fs_frag
    sb.min_free_percent = read_be_i32(data, 0x3C);     // fs_minfree
    sb.frag_shift = read_be_i32(data, 0x54);           // fs_fshift
    sb.max_blocks_per_group = read_be_i32(data, 0x5C); // fs_maxbpg
    sb.frag_to_block_shift = read_be_i32(data, 0x60);  // fs_fragshift
    sb.frag_to_disk_shift = read_be_i32(data, 0x64);   // fs_fsbtodb
    sb.sb_size = read_be_i32(data, 0x68);              // fs_sbsize
    sb.optim = read_be_i32(data, 0x80);                // fs_optim
    sb.indirect_per_block = read_be_i32(data, 0x74);   // fs_nindir
    sb.cs_size = read_be_i32(data, 0x9C);              // fs_cssize
    sb.sb_location = read_be_i64(data, 0x3E8);         // fs_sblockloc
    sb.cs_address = read_be_i64(data, 0x448);          // fs_csaddr
    sb.avg_file_size = read_be_i32(data, 0x4AC);       // fs_avgfilesize
    sb.avg_files_per_dir = read_be_i32(data, 0x4B0);   // fs_avgfpdir
    sb.flags = read_be_i32(data, 0x520);               // fs_flags
    sb.contig_sum_size = read_be_i32(data, 0x524);     // fs_contigsumsize

    sb.volume_name = read_fixed_cstr(data, 0x480, 32); // fs_volname
    return sb;
}

inode inode::parse(std::span<const std::byte> data, std::uint64_t number) {
    inode in;
    in.number = number;
    in.mode = read_be_u16(data.data() + 0x00);
    in.type = file_type_from_mode(in.mode);
    in.link_count = static_cast<std::int16_t>(read_be_u16(data.data() + 0x02));
    in.uid = read_be_u32(data, 0x04);
    in.gid = read_be_u32(data, 0x08);
    in.size = read_be_i64(data, 0x10);
    in.blocks = read_be_i64(data, 0x18);

    // ps3 packs the timestamps non interleaved: atime, mtime, ctime, birthtime
    in.atime = read_be_i64(data, 0x20);
    in.mtime = read_be_i64(data, 0x28);
    in.ctime = read_be_i64(data, 0x30);
    in.birthtime = read_be_i64(data, 0x38);

    in.flags = read_be_u32(data, 0x58);

    for (int i = 0; i < 12; ++i)
        in.direct_blocks[i] = read_be_i64(data, 0x70 + i * 8);   // di_db[12]
    in.indirect_block = read_be_i64(data, 0xD0);                 // di_ib[0]
    in.double_indirect_block = read_be_i64(data, 0xD8);          // di_ib[1]
    in.triple_indirect_block = read_be_i64(data, 0xE0);          // di_ib[2]
    return in;
}

std::vector<directory_entry> parse_directory(std::span<const std::byte> data) {
    std::vector<directory_entry> entries;
    std::size_t offset = 0;
    while (offset + 8 <= data.size()) {
        const std::uint32_t ino = read_be_u32(data, offset);
        const std::uint16_t rec_len = read_be_u16(data.data() + offset + 4);
        const auto d_type = static_cast<dirent_type>(std::to_integer<std::uint8_t>(data[offset + 6]));
        const std::uint8_t name_len = std::to_integer<std::uint8_t>(data[offset + 7]);

        if (rec_len == 0) break; // malformed / end

        if (ino != 0 && name_len > 0 && offset + 8 + name_len <= data.size()) {
            directory_entry e;
            e.inode_number = ino;
            e.record_length = rec_len;
            e.type = d_type;
            e.name.assign(reinterpret_cast<const char*>(data.data() + offset + 8), name_len);
            entries.push_back(std::move(e));
        }
        offset += rec_len;
    }
    return entries;
}

} // namespace ps3hdd::fs