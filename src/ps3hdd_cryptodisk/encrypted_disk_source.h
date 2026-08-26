#pragma once

#include <ps3hdd_crypto/aes_xts_128.h>
#include <ps3hdd_disk/disk_source.h>

#include <cstdint>
#include <memory>
#include <span>

namespace ps3hdd::cryptodisk {

class encrypted_disk_source : public disk::disk_source {
public:
    encrypted_disk_source(std::shared_ptr<disk::disk_source> inner, std::span<const std::byte> data_key, std::span<const std::byte> tweak_key, bool apply_bswap16 = true);

    std::uint64_t total_size() const override { return inner_->total_size(); }
    std::uint32_t sector_size() const override { return inner_->sector_size(); }
    std::string description() const override { return "Decrypted: " + inner_->description(); }
    bool can_write() const override { return inner_->can_write(); }

    std::vector<std::byte> read_sectors(std::uint64_t start_sector, std::uint64_t count) override;
    std::vector<std::byte> read_bytes(std::uint64_t offset, std::size_t count) override;
    void write_sectors(std::uint64_t start_sector, std::span<const std::byte> plaintext) override;
    void write_bytes(std::uint64_t offset, std::span<const std::byte> data) override;

    disk::disk_source& inner() { return *inner_; }

private:
    std::shared_ptr<disk::disk_source> inner_;
    crypto::aes_xts_128 cipher_;
    bool apply_bswap16_;
};

} // namespace ps3hdd::cryptodisk