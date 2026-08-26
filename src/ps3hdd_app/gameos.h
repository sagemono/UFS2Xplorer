#pragma once

#include <ps3hdd_crypto/ps3_key_derivation.h>
#include <ps3hdd_disk/disk_source.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>

namespace ps3hdd::app {

struct gameos_mount {
    std::shared_ptr<disk::disk_source> decrypted;
    std::uint64_t partition_sector = 0;
    std::string cipher;
    bool bswap16 = false;
    int cylinder_groups = 0;
};

std::optional<gameos_mount> open_gameos(std::shared_ptr<disk::disk_source> raw, const crypto::ata_xts_keys& xts, std::span<const std::byte> cbc);
std::optional<gameos_mount> open_gameos(std::shared_ptr<disk::disk_source> raw, std::span<const std::byte> eid_root_key);

} // namespace ps3hdd::app