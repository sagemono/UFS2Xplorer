#include "encrypted_disk_source_cbc.h"

#include <ps3hdd_crypto/bswap16.h>

#include <cstring>
#include <stdexcept>

namespace ps3hdd::cryptodisk {

encrypted_disk_source_cbc::encrypted_disk_source_cbc(std::shared_ptr<disk::disk_source> inner, std::span<const std::byte> key, bool apply_bswap16) : inner_(std::move(inner)), cipher_(key), apply_bswap16_(apply_bswap16) {
    if (!inner_) throw std::invalid_argument("encrypted_disk_source_cbc: null inner source");
}

std::vector<std::byte> encrypted_disk_source_cbc::read_sectors(std::uint64_t start_sector, std::uint64_t count) {
    auto enc = inner_->read_sectors(start_sector, count);
    if (apply_bswap16_) crypto::bswap16_in_place(enc);
    std::vector<std::byte> plain(enc.size());
    cipher_.decrypt_sectors(enc, plain); // zero IV per sector, no tweak
    return plain;
}

std::vector<std::byte> encrypted_disk_source_cbc::read_bytes(std::uint64_t offset, std::size_t count) {
    if (offset + count > total_size())
        throw std::out_of_range("encrypted read past end of disk");
    const std::uint32_t ss = sector_size();
    const std::uint64_t start_sector = offset / ss;
    const std::size_t sector_off = static_cast<std::size_t>(offset % ss);
    const std::uint64_t sectors = (sector_off + count + ss - 1) / ss;

    auto plain = read_sectors(start_sector, sectors);
    std::vector<std::byte> result(count, std::byte{0});
    if (sector_off < plain.size())
        std::memcpy(result.data(), plain.data() + sector_off, std::min(count, plain.size() - sector_off));
    return result;
}

void encrypted_disk_source_cbc::write_sectors(std::uint64_t start_sector, std::span<const std::byte> plaintext) {
    std::vector<std::byte> enc(plaintext.size());
    cipher_.encrypt_sectors(plaintext, enc);
    if (apply_bswap16_) crypto::bswap16_in_place(enc);
    inner_->write_sectors(start_sector, enc);
}

void encrypted_disk_source_cbc::write_bytes(std::uint64_t offset, std::span<const std::byte> data) {
    if (offset + data.size() > total_size())
        throw std::out_of_range("encrypted write past end of disk");
    if (offset % sector_size() != 0 || data.size() % sector_size() != 0)
        throw std::invalid_argument("encrypted writes must be sector-aligned");
    write_sectors(offset / sector_size(), data);
}

} // namespace ps3hdd::cryptodisk