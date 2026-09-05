#pragma once

#include "ps3_pkg_reader.h"

#include <ps3hdd_fs/ufs2_filesystem.h>
#include <ps3hdd_fs/ufs2_writer.h>

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace ps3hdd::pkg {

class pkg_installer {
public:
    pkg_installer(fs::ufs2_filesystem& filesystem, fs::ufs2_writer& writer, const ps3_pkg_reader& pkg);

    using file_progress = std::function<void(const std::string& name, int index, int count)>;
    using byte_progress = std::function<void(std::int64_t bytes)>;

    std::string title_id() const { return pkg_.title_id(); }
    std::uint64_t total_bytes() const;
    std::string install(const file_progress& progress = {}, const byte_progress& on_written = {});

private:
    struct dir_node {
        std::map<std::string, dir_node> dirs;
        std::vector<std::pair<std::string, const pkg_entry*>> files;
    };

    fs::ufs2_filesystem& fs_;
    fs::ufs2_writer& writer_;
    const ps3_pkg_reader& pkg_;

    dir_node build_tree() const;
    std::int64_t resolve_child(std::uint64_t parent, const std::string& name) const;
    std::uint64_t ensure_directory(std::uint64_t parent, const std::string& name, const std::string& path);
    void install_node(const dir_node& node, std::uint64_t dir_inode, const std::string& path, int total, int& done, const file_progress& progress, const byte_progress& on_written);
};

} // namespace ps3hdd::pkg