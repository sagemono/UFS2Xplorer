#pragma once

#include "ufs2_types.h"

#include <ps3hdd_disk/disk_source.h>

#include <cstdint>
#include <functional>
#include <optional>
#include <string_view>
#include <vector>

namespace ps3hdd::fs {

class ufs2_filesystem {
public:
    ufs2_filesystem(disk::disk_source& disk, std::uint64_t partition_start_sector);

    bool mount();

    const superblock& sb() const { return sb_; }
    std::uint64_t partition_offset_bytes() const { return partition_offset_; }

    inode read_inode(std::uint64_t inode_number);
    std::vector<std::byte> read_inode_raw(std::uint64_t inode_number);
    std::vector<directory_entry> read_directory(const inode& dir);

    std::vector<std::byte> read_inode_data(const inode& in);
    void extract_inode(const inode& in, const std::function<void(std::span<const std::byte>)>& sink, const std::function<void(std::uint64_t)>& progress = {});

    std::vector<std::int64_t> block_pointers(const inode& in) { return all_block_pointers(in); }
    std::vector<std::int64_t> block_pointers_with_holes(const inode& in);
    void read_range(const std::vector<std::int64_t>& blocks, std::uint64_t file_offset, std::span<std::byte> out);

    std::optional<inode> resolve_path(std::string_view path);
    std::optional<std::uint64_t> resolve_path_to_inode_number(std::string_view path);

    static constexpr std::uint64_t root_inode = 2;

private:
    disk::disk_source& disk_;
    std::uint64_t partition_offset_ = 0;
    superblock sb_;

    std::vector<std::byte> read_block(std::int64_t fragment_address);
    void collect_block_pointers(std::int64_t block_addr, int level, std::vector<std::int64_t>& out);
    std::vector<std::int64_t> all_block_pointers(const inode& in);
};

} // namespace ps3hdd::fs