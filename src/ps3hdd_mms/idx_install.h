#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace ps3hdd::mms {

struct install_keys {
    std::string title_id;
    std::string title_sort;
    std::string date;
    std::string owner;
    std::string status;
    std::string dir_path;
    std::string ff;
};

enum class objref_family { flat, tree, shed, append_entry };

std::uint32_t ssize(std::uint32_t m);
std::vector<std::uint32_t> tree_trailer(std::uint32_t n);
std::vector<std::uint32_t> shed_trailer(std::uint32_t n);
std::vector<std::byte> gen_leaf_add(std::span<const std::byte> leaf, const std::string& key,bool inline_summary);
std::vector<std::byte> gen_leaf_bump(std::span<const std::byte> leaf, const std::string& key, bool inline_summary);
std::vector<std::byte> gen_objref(std::span<const std::byte> block, objref_family fam,std::uint32_t new_obj);
std::vector<std::byte> install_into_idx(std::span<const std::byte> idx, const install_keys& keys, std::uint32_t new_obj);

struct container_write {
    std::vector<std::byte> record;
    std::size_t record_at;
    std::size_t objentry_at;
    std::uint32_t timestamp;
    std::uint32_t heap_base;
    std::uint32_t obj_off;
    std::uint32_t heap_alloc;
};

std::vector<std::byte> install_into_container(std::span<const std::byte> container, std::uint32_t new_obj, const container_write& w);

} // namespace ps3hdd::mms