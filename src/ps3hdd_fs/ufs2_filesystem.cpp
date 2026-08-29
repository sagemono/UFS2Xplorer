#include "ufs2_filesystem.h"

#include <ps3hdd_crypto/be_io.h>

#include <algorithm>
#include <stdexcept>

namespace ps3hdd::fs {

ufs2_filesystem::ufs2_filesystem(disk::disk_source& disk, std::uint64_t partition_start_sector) : disk_(disk), partition_offset_(partition_start_sector * 512) {}

bool ufs2_filesystem::mount() {
    auto data = disk_.read_bytes(partition_offset_ + 65536, 8192);
    sb_ = superblock::parse(data);
    return sb_.valid();
}

inode ufs2_filesystem::read_inode(std::uint64_t inode_number) {
    if (!sb_.valid()) throw std::runtime_error("Filesystem not mounted.");

    const std::int64_t group = static_cast<std::int64_t>(inode_number) / sb_.inodes_per_group;
    const std::int64_t index = static_cast<std::int64_t>(inode_number) % sb_.inodes_per_group;

    const std::uint64_t cg_offset = partition_offset_ + static_cast<std::uint64_t>(group) * sb_.frags_per_group * sb_.fragment_size;
    const std::uint64_t inode_table = cg_offset + sb_.inode_block_offset * sb_.fragment_size;
    const std::uint64_t inode_offset = inode_table + static_cast<std::uint64_t>(index) * superblock::inode_size;

    auto data = disk_.read_bytes(inode_offset, superblock::inode_size);
    return inode::parse(data, inode_number);
}

std::vector<std::byte> ufs2_filesystem::read_block(std::int64_t fragment_address) {
    const std::uint64_t offset = partition_offset_ + static_cast<std::uint64_t>(fragment_address) * sb_.fragment_size;
    if (offset + static_cast<std::uint64_t>(sb_.block_size) > disk_.total_size())
        throw std::runtime_error("read_block: address out of disk bounds");
    return disk_.read_bytes(offset, static_cast<std::size_t>(sb_.block_size));
}

void ufs2_filesystem::collect_block_pointers(std::int64_t block_addr, int level, std::vector<std::int64_t>& out) {
    if (block_addr == 0) return;
    auto blk = read_block(block_addr);
    const std::size_t ptrs = static_cast<std::size_t>(sb_.block_size / 8);
    for (std::size_t i = 0; i < ptrs; ++i) {
        const std::int64_t p = static_cast<std::int64_t>(ps3hdd::read_be_u64(blk.data() + i * 8));
        if (p == 0) continue;
        if (level == 1) out.push_back(p);
        else collect_block_pointers(p, level - 1, out);
    }
}

std::vector<std::int64_t> ufs2_filesystem::all_block_pointers(const inode& in) {
    std::vector<std::int64_t> ptrs;
    for (int i = 0; i < 12; ++i) {
        if (in.direct_blocks[i] == 0) break;
        ptrs.push_back(in.direct_blocks[i]);
    }
    if (in.indirect_block) collect_block_pointers(in.indirect_block, 1, ptrs);
    if (in.double_indirect_block) collect_block_pointers(in.double_indirect_block, 2, ptrs);
    if (in.triple_indirect_block) collect_block_pointers(in.triple_indirect_block, 3, ptrs);
    return ptrs;
}

void ufs2_filesystem::extract_inode(const inode& in, const std::function<void(std::span<const std::byte>)>& sink, const std::function<void(std::uint64_t)>& progress) {
    if (in.size == 0) return;
    const std::uint64_t file_size = static_cast<std::uint64_t>(in.size);
    const std::int64_t frags_per_block = sb_.block_size / sb_.fragment_size;
    const auto ptrs = all_block_pointers(in);

    std::uint64_t written = 0;
    std::size_t idx = 0;
    while (idx < ptrs.size() && written < file_size) {
        const std::int64_t run_start = ptrs[idx];
        std::size_t run_blocks = 1;
        while (idx + run_blocks < ptrs.size() && ptrs[idx + run_blocks] == run_start + static_cast<std::int64_t>(run_blocks) * frags_per_block) 
        ++run_blocks;

        const std::uint64_t offset = partition_offset_ + static_cast<std::uint64_t>(run_start) * sb_.fragment_size;
        const std::uint64_t run_bytes = run_blocks * static_cast<std::uint64_t>(sb_.block_size);
        const std::uint64_t to_emit = std::min<std::uint64_t>(run_bytes, file_size - written);


        constexpr std::uint64_t kChunk = 8u * 1024 * 1024;
        std::uint64_t done = 0;
        while (done < to_emit) {
            const std::size_t piece = static_cast<std::size_t>(std::min<std::uint64_t>(kChunk, to_emit - done));
            auto data = disk_.read_bytes(offset + done, piece);
            sink({data.data(), data.size()});
            done += piece;
            if (progress) progress(written + done);
        }
        written += to_emit;
        idx += run_blocks;
    }
}

std::vector<std::byte> ufs2_filesystem::read_inode_data(const inode& in) {
    std::vector<std::byte> out;
    out.reserve(static_cast<std::size_t>(in.size));
    extract_inode(in, [&](std::span<const std::byte> chunk) {
        out.insert(out.end(), chunk.begin(), chunk.end());
    });
    return out;
}

std::vector<directory_entry> ufs2_filesystem::read_directory(const inode& dir) {
    if (!dir.is_directory()) throw std::invalid_argument("read_directory: inode is not a directory");
    auto data = read_inode_data(dir);
    return parse_directory(data);
}

std::optional<std::uint64_t> ufs2_filesystem::resolve_path_to_inode_number(std::string_view path) {
    while (!path.empty() && path.front() == '/') path.remove_prefix(1);
    while (!path.empty() && path.back() == '/') path.remove_suffix(1);
    if (path.empty()) return root_inode;

    std::uint64_t current = root_inode;
    std::size_t start = 0;
    while (start <= path.size()) {
        const std::size_t slash = path.find('/', start);
        const std::string_view part =
            path.substr(start, slash == std::string_view::npos ? std::string_view::npos : slash - start);
        if (!part.empty()) {
            const inode dir = read_inode(current);
            if (!dir.is_directory()) return std::nullopt;
            const auto entries = read_directory(dir);
            auto it = std::find_if(entries.begin(), entries.end(), [&](const directory_entry& e) { return e.name == part; });
            if (it == entries.end()) return std::nullopt;
            current = it->inode_number;
        }
        if (slash == std::string_view::npos) break;
        start = slash + 1;
    }
    return current;
}

std::optional<inode> ufs2_filesystem::resolve_path(std::string_view path) {
    auto num = resolve_path_to_inode_number(path);
    if (!num) return std::nullopt;
    return read_inode(*num);
}

} // namespace ps3hdd::fs