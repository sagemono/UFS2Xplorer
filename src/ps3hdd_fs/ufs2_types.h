#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace ps3hdd::fs {

enum class file_type {
    unknown, fifo, char_device, directory, block_device,
    regular_file, symbolic_link, socket
};

enum class dirent_type : std::uint8_t {
    unknown = 0, fifo = 1, char_device = 2, directory = 4,
    block_device = 6, regular_file = 8, symbolic_link = 10,
    socket = 12, whiteout = 14
};

struct superblock {
    static constexpr std::uint32_t magic_value = 0x19540119u;
    static constexpr std::size_t magic_offset = 0x55C;   // fs_magic
    static constexpr std::uint32_t inode_size = 256;     // UFS2 dinode is always 256 bytes

    std::uint32_t magic = 0;
    std::int64_t block_size = 0;           // fs_bsize
    std::int64_t fragment_size = 0;        // fs_fsize
    std::int64_t frags_per_group = 0;      // fs_fpg
    std::int64_t inodes_per_group = 0;     // fs_ipg
    std::int64_t inode_block_offset = 0;   // fs_iblkno (in fragments)
    std::int64_t cg_block_offset = 0;      // fs_cblkno (cylinder-group header, fragments)
    std::int64_t data_block_offset = 0;    // fs_dblkno (first data block, fragments)
    std::int32_t cg_size = 0;              // fs_cgsize (bytes of a CG header block)
    std::int64_t total_fragments = 0;      // fs_size
    std::int64_t total_data_fragments = 0; // fs_dsize
    std::int32_t cylinder_groups = 0;      // fs_ncg
    std::string volume_name;

    std::int64_t directories = 0;          // cs_ndir
    std::int64_t free_blocks = 0;          // cs_nbfree
    std::int64_t free_inodes = 0;          // cs_nifree
    std::int64_t free_fragments = 0;       // cs_nffree

    // fields lv2s allocator reads. offsets confirmed in ida
    std::int32_t frag = 0;                 // 0x38  fs_frag fragments per block
    std::int32_t min_free_percent = 0;     // 0x3C  fs_minfree
    std::int32_t frag_shift = 0;           // 0x54  fs_fshift bytes -> fragments
    std::int32_t max_blocks_per_group = 0; // 0x5C  fs_maxbpg
    std::int32_t frag_to_block_shift = 0;  // 0x60  fs_fragshift fragments -> blocks
    std::int32_t frag_to_disk_shift = 0;   // 0x64  fs_fsbtodb
    std::int32_t sb_size = 0;              // 0x68  fs_sbsize
    // 0 = FS_OPTTIME, 1 = FS_OPTSPACE. //unlock_hdd_space.h
    std::int32_t optim = 0;                // 0x80  fs_optim
    std::int32_t cs_size = 0;              // 0x9C  fs_cssize bytes of the cs array
    std::int32_t indirect_per_block = 0;   // 0x74  fs_nindir
    std::int64_t sb_location = 0;          // 0x3E8 fs_sblockloc
    std::int64_t cs_address = 0;           // 0x448 fs_csaddr fragment addr of cs array
    std::int32_t avg_file_size = 0;        // 0x4AC fs_avgfilesize
    std::int32_t avg_files_per_dir = 0;    // 0x4B0 fs_avgfpdir
    std::int32_t flags = 0;                // 0x520 fs_flags
    std::int32_t contig_sum_size = 0;      // 0x524 fs_contigsumsize

    static constexpr int direct_blocks = 12;     // NDADDR
    static constexpr int dir_block_size = 512;   // DIRBLKSIZ

    bool valid() const { return magic == magic_value; }
    std::int64_t free_space_bytes() const {
        return free_blocks * block_size + free_fragments * fragment_size;
    }

    static superblock parse(std::span<const std::byte> data);
};

struct inode {
    std::uint64_t number = 0;
    file_type type = file_type::unknown;
    std::uint16_t mode = 0;
    std::int16_t link_count = 0;
    std::uint32_t uid = 0;
    std::uint32_t gid = 0;
    std::int64_t size = 0;
    std::int64_t blocks = 0;
    std::int64_t atime = 0, mtime = 0, ctime = 0, birthtime = 0;
    std::uint32_t flags = 0;
    std::array<std::int64_t, 12> direct_blocks{};   // di_db
    std::int64_t indirect_block = 0;                // di_ib[0]
    std::int64_t double_indirect_block = 0;         // di_ib[1]
    std::int64_t triple_indirect_block = 0;         // di_ib[2]

    bool is_directory() const { return type == file_type::directory; }

    static inode parse(std::span<const std::byte> data, std::uint64_t number);
};

struct directory_entry {
    std::uint32_t inode_number = 0;
    std::string name;
    dirent_type type = dirent_type::unknown;
    std::uint16_t record_length = 0;
};

std::vector<directory_entry> parse_directory(std::span<const std::byte> data);

file_type file_type_from_mode(std::uint16_t mode);

} // namespace ps3hdd::fs