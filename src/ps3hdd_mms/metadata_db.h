#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ps3hdd::mms {

std::uint32_t crc32_mpeg2(std::span<const std::byte> data);

std::uint16_t crc16_ccitt(std::span<const std::byte> data, std::uint16_t seed = 0x10FA);

inline constexpr std::size_t kPageSize = 0x2000;
inline constexpr std::size_t kNodeSize = 0x1000;
inline constexpr std::array<std::byte, 8> kContainerMagic = {
    std::byte{0xb9}, std::byte{0x8d}, std::byte{0x9a}, std::byte{0x9e},
    std::byte{0x94}, std::byte{0xaf}, std::byte{0xac}, std::byte{0xcc}};

class game_record {
public:
    static game_record parse(std::span<const std::byte> img, std::size_t base, std::size_t end);

    std::vector<std::byte> serialize() const;

    std::string get_string(std::size_t slot) const;
    void set_string(std::size_t slot, std::string_view utf8);
    void copy_scalar_block(std::span<const std::byte> other_fixed);
    std::size_t slot_count() const { return slots_.size(); }
    std::span<const std::byte> fixed() const { return fixed_; }

private:
    struct slot { std::uint16_t off; std::uint16_t len; std::vector<std::byte> heap; bool is_heap; };
    static constexpr std::size_t kPtrTable = 0x125;
    std::vector<std::byte> fixed_;
    std::uint16_t heap_start_ = 0;
    std::size_t rec_len_ = 0;
    std::vector<slot> slots_;
};

class btree_leaf {
public:
    static btree_leaf parse(std::span<const std::byte> node);
    const std::vector<std::string>& keys() const { return keys_; }
    std::uint32_t count() const { return static_cast<std::uint32_t>(keys_.size()); }
    bool contains(std::string_view titleId) const;
    bool insert(std::string titleId);
    bool remove(std::string_view titleId);
    std::vector<std::byte> serialize() const;

private:
    static constexpr std::size_t kUsed = 0x800, kCount = 0x804, kCrc = 0x81c, kKeys = 0x820;
    std::vector<std::byte> node_;
    std::vector<std::string> keys_;
};

bool verify_idx_crcs(std::span<const std::byte> idx, std::size_t* bad_offset = nullptr);

struct game_info {
    std::size_t offset = 0;
    std::string title_id;
    std::string category;
    std::string version;
    std::string ps3_system_ver;
    std::string title;
    std::string dir_path;
};

std::vector<game_info> enumerate_games(std::span<const std::byte> container);

struct db_report {
    bool container_magic_ok = false;
    bool idx_crcs_ok = false;
    std::size_t idx_bad_offset = 0;
    int index_trees = 0;
    int indexed_keys = 0;
    std::size_t master_index_offset = 0;
    std::vector<game_info> games;
};

db_report analyze(std::span<const std::byte> container, std::span<const std::byte> idx);

} // namespace ps3hdd::mms