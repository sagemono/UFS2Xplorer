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