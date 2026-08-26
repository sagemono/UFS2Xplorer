#pragma once

#include <ps3hdd_crypto/aes_ctr_128.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace ps3hdd::pkg {

enum class crypto_mode { aes_ctr, sha1_xor };

struct pkg_entry {
    std::string name;
    std::uint64_t name_offset = 0;
    std::uint32_t name_size = 0;
    std::uint64_t data_offset = 0;
    std::uint64_t data_size = 0;
    std::uint32_t type_flags = 0;
    std::uint8_t file_type = 0;
    bool overwrite = false;
    bool psp = false;
    bool is_directory = false;
};

class ps3_pkg_reader {
public:
    using read_fn = std::function<void(std::uint64_t offset, std::span<std::byte> out)>;

    ps3_pkg_reader(read_fn read, std::uint64_t file_size);

    static ps3_pkg_reader from_memory(std::span<const std::byte> data);
    static ps3_pkg_reader from_file(const std::string& path);

    bool finalized() const { return finalized_; }
    crypto_mode mode() const { return mode_; }
    const std::string& content_id() const { return content_id_; }
    std::uint32_t content_type() const { return content_type_; }
    std::uint32_t drm_type() const { return drm_type_; }
    std::string title_id() const;
    const std::vector<pkg_entry>& entries() const { return entries_; }

    void decrypt_range(const pkg_entry& entry, std::uint64_t entry_offset, std::span<std::byte> out) const;

private:
    read_fn read_;
    std::uint64_t file_size_ = 0;

    bool finalized_ = false;
    crypto_mode mode_ = crypto_mode::aes_ctr;
    std::uint32_t file_count_ = 0;
    std::uint64_t data_offset_ = 0;
    std::uint64_t data_size_ = 0;
    std::uint32_t content_type_ = 0;
    std::uint32_t drm_type_ = 0;
    std::string content_id_;
    std::array<std::byte, 16> riv_{};
    std::array<std::byte, 0x40> sha1_base_{};
    std::unique_ptr<crypto::aes_ctr_128> ctr_;
    std::unique_ptr<crypto::aes_ctr_128> ctr2_;
    bool psp_platform_ = false;
    std::vector<pkg_entry> entries_;

    void parse_header();
    void parse_file_table();
    void init_crypto();
    void decrypt(std::uint64_t data_rel, std::span<const std::byte> in, std::span<std::byte> out, bool key2 = false) const;
};

} // namespace ps3hdd::pkg