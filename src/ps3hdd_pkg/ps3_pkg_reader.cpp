#include "ps3_pkg_reader.h"

#include <ps3hdd_crypto/be_io.h>
#include <ps3hdd_crypto/sha1_xor.h>

#include <filesystem>
#include <fstream>
#include <cstring>
#include <stdexcept>

namespace ps3hdd::pkg {

namespace {
constexpr std::array<std::byte, 16> kPs3AesKey = {
    std::byte{0x2E}, std::byte{0x7B}, std::byte{0x71}, std::byte{0xD7},
    std::byte{0xC9}, std::byte{0xC9}, std::byte{0xA1}, std::byte{0x4E},
    std::byte{0xA3}, std::byte{0x22}, std::byte{0x1F}, std::byte{0x18},
    std::byte{0x88}, std::byte{0x28}, std::byte{0xB8}, std::byte{0xF8}
};

constexpr std::array<std::byte, 16> kPspAesKey = {
    std::byte{0x07}, std::byte{0xF2}, std::byte{0xC6}, std::byte{0x82},
    std::byte{0x90}, std::byte{0xB5}, std::byte{0x0D}, std::byte{0x2C},
    std::byte{0x33}, std::byte{0x81}, std::byte{0x8D}, std::byte{0x70},
    std::byte{0x9B}, std::byte{0x60}, std::byte{0xE6}, std::byte{0x2B}
};
} // namespace

ps3_pkg_reader::ps3_pkg_reader(read_fn read, std::uint64_t file_size)
    : read_(std::move(read)), file_size_(file_size) {
    parse_header();
    init_crypto();
    parse_file_table();
}

ps3_pkg_reader ps3_pkg_reader::from_memory(std::span<const std::byte> data) {
    auto buf = std::make_shared<std::vector<std::byte>>(data.begin(), data.end());
    read_fn read = [buf](std::uint64_t offset, std::span<std::byte> out) {
        std::fill(out.begin(), out.end(), std::byte{0});
        if (offset < buf->size()) {
            const std::size_t n = std::min<std::size_t>(out.size(), buf->size() - offset);
            std::memcpy(out.data(), buf->data() + offset, n);
        }
    };
    return ps3_pkg_reader(std::move(read), data.size());
}

ps3_pkg_reader ps3_pkg_reader::from_file(const std::string& path) {
    const std::filesystem::path fp(reinterpret_cast<const char8_t*>(path.c_str()));
    auto stream = std::make_shared<std::ifstream>(fp, std::ios::binary);
    if (!*stream) throw std::runtime_error("cannot open PKG file: " + path);
    stream->seekg(0, std::ios::end);
    const std::uint64_t size = static_cast<std::uint64_t>(stream->tellg());
    read_fn read = [stream](std::uint64_t offset, std::span<std::byte> out) {
        std::fill(out.begin(), out.end(), std::byte{0});
        stream->clear();
        stream->seekg(static_cast<std::streamoff>(offset), std::ios::beg);
        stream->read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(out.size()));
    };
    return ps3_pkg_reader(std::move(read), size);
}

void ps3_pkg_reader::parse_header() {
    std::array<std::byte, 0x80> h{};
    read_(0, h);
    if (!(h[0] == std::byte{0x7F} && h[1] == std::byte{0x50} &&
          h[2] == std::byte{0x4B} && h[3] == std::byte{0x47}))
        throw std::runtime_error("not a PS3 PKG (bad magic)");

    finalized_ = (std::to_integer<int>(h[4]) & 0x80) != 0;
    mode_ = finalized_ ? crypto_mode::aes_ctr : crypto_mode::sha1_xor;
    if (h[7] == std::byte{0x02})
        psp_platform_ = true;
    else if (h[7] != std::byte{0x01})
        throw std::runtime_error("not a PS3 PKG (unsupported platform)");

    file_count_ = ps3hdd::read_be_u32(h.data() + 0x14);
    data_offset_ = ps3hdd::read_be_u64(h.data() + 0x20);
    data_size_ = ps3hdd::read_be_u64(h.data() + 0x28);
    content_id_ = ps3hdd::read_cstr({h.data(), h.size()}, 0x30, 48);
    std::memcpy(riv_.data(), h.data() + 0x70, 16);

    const std::uint32_t meta_off = ps3hdd::read_be_u32(h.data() + 0x08);
    const std::uint32_t meta_cnt = ps3hdd::read_be_u32(h.data() + 0x0C);
    if (meta_off >= 0x80 && meta_off < data_offset_ && meta_cnt > 0 && meta_cnt < 256) {
        std::uint64_t span = data_offset_ - meta_off;
        if (span > 0x10000) span = 0x10000;
        std::vector<std::byte> meta(static_cast<std::size_t>(span));
        read_(meta_off, meta);
        std::size_t p = 0;
        for (std::uint32_t i = 0; i < meta_cnt && p + 8 <= meta.size(); ++i) {
            const std::uint32_t id = ps3hdd::read_be_u32(meta.data() + p);
            const std::uint32_t sz = ps3hdd::read_be_u32(meta.data() + p + 4);
            p += 8;
            if (p + sz > meta.size()) break;
            if (id == 0x01 && sz >= 4) drm_type_ = ps3hdd::read_be_u32(meta.data() + p);
            if (id == 0x02 && sz >= 4) content_type_ = ps3hdd::read_be_u32(meta.data() + p);
            p += sz;
        }
    }

    sha1_base_ = crypto::sha1_xor_base_key({h.data() + 0x60, 16});
}

void ps3_pkg_reader::init_crypto() {
    ctr_ = std::make_unique<crypto::aes_ctr_128>(std::span<const std::byte>(kPs3AesKey));
    ctr2_ = std::make_unique<crypto::aes_ctr_128>(std::span<const std::byte>(kPspAesKey));
}

void ps3_pkg_reader::decrypt(std::uint64_t data_rel, std::span<const std::byte> in, std::span<std::byte> out, bool key2) const {
    if (mode_ == crypto_mode::aes_ctr)
        (key2 ? ctr2_ : ctr_)->process(in, out, riv_, data_rel / 16);
    else
        crypto::sha1_xor(sha1_base_, data_rel, in, out);
}

void ps3_pkg_reader::parse_file_table() {
    const std::size_t table_bytes = static_cast<std::size_t>(file_count_) * 32;
    std::vector<std::byte> enc(table_bytes);
    read_(data_offset_, enc);
    std::vector<std::byte> table(table_bytes);
    decrypt(0, enc, table, psp_platform_);

    if (file_count_ > 0) {
        const std::uint32_t ns = ps3hdd::read_be_u32(table.data() + 4);
        if (ns == 0 || ns > 4096) {
            mode_ = (mode_ == crypto_mode::aes_ctr) ? crypto_mode::sha1_xor : crypto_mode::aes_ctr;
            decrypt(0, enc, table, psp_platform_);
            const std::uint32_t ns2 = ps3hdd::read_be_u32(table.data() + 4);
            if (ns2 == 0 || ns2 > 4096)
                throw std::runtime_error("PKG file table is corrupt or unsupported");
        }
    }

    for (std::uint32_t i = 0; i < file_count_; ++i) {
        const std::size_t o = static_cast<std::size_t>(i) * 32;
        pkg_entry e;
        e.name_offset = ps3hdd::read_be_u32(table.data() + o);
        e.name_size = ps3hdd::read_be_u32(table.data() + o + 4);
        e.data_offset = ps3hdd::read_be_u64(table.data() + o + 8);
        e.data_size = ps3hdd::read_be_u64(table.data() + o + 16);
        e.type_flags = ps3hdd::read_be_u32(table.data() + o + 24);
        e.file_type = std::to_integer<std::uint8_t>(table[o + 27]);
        e.overwrite = (e.type_flags & 0x80000000u) != 0;
        e.psp = (e.type_flags & 0x10000000u) != 0;
        if (e.name_size == 0 || e.name_size > 4096) continue;

        std::vector<std::byte> enc_name(e.name_size);
        read_(data_offset_ + e.name_offset, enc_name);
        std::vector<std::byte> name(e.name_size);
        decrypt(e.name_offset, enc_name, name, e.psp);
        e.name.assign(reinterpret_cast<const char*>(name.data()), e.name_size);
        while (!e.name.empty() && e.name.back() == '\0') e.name.pop_back();

        e.is_directory = (e.file_type & 0x0F) == 0x04 && e.data_size == 0;
        entries_.push_back(std::move(e));
    }
}

void ps3_pkg_reader::decrypt_range(const pkg_entry& entry, std::uint64_t entry_offset, std::span<std::byte> out) const {
    const std::uint64_t data_rel = entry.data_offset + entry_offset;
    std::vector<std::byte> enc(out.size());
    read_(data_offset_ + data_rel, enc);
    decrypt(data_rel, enc, out, entry.psp);
}

std::string ps3_pkg_reader::title_id() const {
    const std::size_t dash = content_id_.find('-');
    if (dash == std::string::npos) return {};
    const std::string rest = content_id_.substr(dash + 1);
    const std::size_t us = rest.find('_');
    return us == std::string::npos ? rest : rest.substr(0, us);
}

} // namespace ps3hdd::pkg