#include "pkg_installer.h"

#include <algorithm>
#include <stdexcept>

namespace ps3hdd::pkg {

pkg_installer::pkg_installer(fs::ufs2_filesystem& filesystem, fs::ufs2_writer& writer, const ps3_pkg_reader& pkg) : fs_(filesystem), writer_(writer), pkg_(pkg) {}

pkg_installer::dir_node pkg_installer::build_tree() const {
    dir_node root;
    for (const auto& e : pkg_.entries()) {
        std::vector<std::string> parts;
        std::string cur;
        for (char c : e.name) {
            if (c == '/' || c == '\\') {
                if (!cur.empty()) parts.push_back(cur);
                cur.clear();
            } else {
                cur.push_back(c);
            }
        }
        if (!cur.empty()) parts.push_back(cur);
        if (parts.empty()) continue;

        dir_node* node = &root;
        for (std::size_t i = 0; i + 1 < parts.size(); ++i)
            node = &node->dirs[parts[i]];

        const std::string& leaf = parts.back();
        if (e.is_directory)
            (void)node->dirs[leaf];
        else
            node->files.emplace_back(leaf, &e);
    }
    return root;
}

std::int64_t pkg_installer::resolve_child(std::uint64_t parent, const std::string& name) const {
    const auto dir = fs_.read_inode(parent);
    for (const auto& e : fs_.read_directory(dir))
        if (e.name == name) return static_cast<std::int64_t>(e.inode_number);
    return -1;
}

std::uint64_t pkg_installer::ensure_directory(std::uint64_t parent, const std::string& name, const std::string& path) {
    const std::int64_t existing = resolve_child(parent, name);
    if (existing >= 0) {
        const auto in = fs_.read_inode(static_cast<std::uint64_t>(existing));
        if (!in.is_directory())
            throw std::runtime_error("cannot install into " + path + name + "/: a file of that name is already on the disk (inode " + std::to_string(existing) + "). Delete it, or uninstall the title, then install again.");
        return static_cast<std::uint64_t>(existing);
    }
    return writer_.create_directory(parent, name);
}

void pkg_installer::install_node(const dir_node& node, std::uint64_t dir_inode, const std::string& path, int total, int& done, const file_progress& progress, const byte_progress& on_written) {
    for (const auto& [name, entry] : node.files) {
        if (progress) progress(name, done + 1, total);
        const bool exists = resolve_child(dir_inode, name) >= 0;
        if (exists && !entry->overwrite) {
            if (on_written) on_written(static_cast<std::int64_t>(entry->data_size));
            ++done;
            continue;
        }
        if (exists) writer_.delete_tree(dir_inode, name);
        std::uint64_t entry_off = 0;
        writer_.write_file(dir_inode, name, static_cast<std::int64_t>(entry->data_size), [&](std::span<std::byte> dst) {pkg_.decrypt_range(*entry, entry_off, dst);entry_off += dst.size();}, on_written);
        ++done;
    }
    for (const auto& [name, child] : node.dirs) {
        const std::uint64_t child_inode = ensure_directory(dir_inode, name, path);
        install_node(child, child_inode, path + name + "/", total, done, progress, on_written);
    }
}

std::uint64_t pkg_installer::total_bytes() const {
    std::uint64_t sum = 0;
    for (const auto& e : pkg_.entries())
        if (!e.is_directory) sum += e.data_size;
    return sum;
}

std::string pkg_installer::install(const file_progress& progress, const byte_progress& on_written) {
    const std::string tid = pkg_.title_id();
    if (tid.empty())
        throw std::runtime_error("could not determine TITLE_ID from the package");

    const std::uint64_t game = ensure_directory(fs::ufs2_filesystem::root_inode, "game", "");
    const std::uint64_t title = ensure_directory(game, tid, "game/");

    const dir_node root = build_tree();
    int total = 0;
    for (const auto& e : pkg_.entries())
        if (!e.is_directory) ++total;

    int done = 0;
    install_node(root, title, "game/" + tid + "/", total, done, progress, on_written);

    writer_.update_superblock();
    return "game/" + tid;
}

} // namespace ps3hdd::pkg