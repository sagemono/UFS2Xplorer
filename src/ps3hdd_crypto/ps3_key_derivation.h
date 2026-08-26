#pragma once

#include <array>
#include <cstddef>
#include <span>

namespace ps3hdd::crypto {

struct ata_xts_keys {
    std::array<std::byte, 16> data_key{};
    std::array<std::byte, 16> tweak_key{};
};

ata_xts_keys derive_ata_xts_keys(std::span<const std::byte> eid_root_key);

std::array<std::byte, 24> derive_ata_cbc_key(std::span<const std::byte> eid_root_key);

} // namespace ps3hdd::crypto
