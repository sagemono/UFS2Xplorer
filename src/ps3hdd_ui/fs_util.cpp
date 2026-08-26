#include "fs_util.h"

#include <algorithm>
#include <cctype>

namespace ps3hdd::ui {

std::uint64_t dir_size(fs::ufs2_filesystem& fs, std::uint64_t inode, const std::atomic<bool>* cancel, int depth) {
    if (depth > 128 || (cancel && cancel->load())) return 0;
    std::uint64_t total = 0;
    try {
        for (const auto& e : fs.read_directory(fs.read_inode(inode))) {
            if (e.name == "." || e.name == "..") continue;
            if (cancel && cancel->load()) return total;
            if (e.type == fs::dirent_type::directory)
                total += dir_size(fs, e.inode_number, cancel, depth + 1);
            else {
                try {
                    total += fs.read_inode(e.inode_number).size;
                } catch (...) {
                }
            }
        }
    } catch (...) {
    }
    return total;
}

std::vector<std::string> local_users(fs::ufs2_filesystem& fs) {
    std::vector<std::string> users;
    try {
        if (auto home = fs.resolve_path_to_inode_number("home")) {
            for (const auto& e : fs.read_directory(fs.read_inode(*home))) {
                if (e.type != fs::dirent_type::directory) continue;
                const std::string& n = e.name;
                if (n.size() == 8 && std::all_of(n.begin(), n.end(), [](unsigned char c) { return std::isdigit(c) != 0; }))
                    users.push_back(n);
            }
        }
    } catch (...) {
    }
    return users;
}

std::vector<std::byte> read_child(fs::ufs2_filesystem& fs, std::uint64_t dir_inode, const std::string& name) {
    try {
        for (const auto& e : fs.read_directory(fs.read_inode(dir_inode)))
            if (e.name == name && e.type != fs::dirent_type::directory)
                return fs.read_inode_data(fs.read_inode(e.inode_number));
    } catch (...) {
    }
    return {};
}

} // namespace ps3hdd::ui