#pragma once

#include <ps3hdd_fs/ufs2_filesystem.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace ps3hdd::ui {

std::uint64_t dir_size(fs::ufs2_filesystem& fs, std::uint64_t inode, const std::atomic<bool>* cancel = nullptr, int depth = 0);

std::vector<std::string> local_users(fs::ufs2_filesystem& fs);
std::vector<std::byte> read_child(fs::ufs2_filesystem& fs, std::uint64_t dir_inode, const std::string& name);

} // namespace ps3hdd::ui