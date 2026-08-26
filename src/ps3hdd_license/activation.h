#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

namespace ps3hdd::license {

struct activation {
    std::array<std::byte, 0x1038> act_dat;
    std::array<std::byte, 0x98> rif;
};

activation build_activation(std::span<const std::byte> idps, const std::string& content_id, std::span<const std::byte> klicensee, std::uint64_t account_id);

std::array<std::byte, 0x98> build_rif(std::span<const std::byte> idps, const std::string& content_id, std::span<const std::byte> klicensee, std::span<const std::byte> act_dat);

std::array<std::byte, 16> rif_recover_klicensee(std::span<const std::byte> idps, std::span<const std::byte> act_dat, std::span<const std::byte> rif);

} // namespace ps3hdd::license