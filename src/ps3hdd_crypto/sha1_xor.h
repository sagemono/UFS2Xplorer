#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace ps3hdd::crypto {

std::array<std::byte, 0x40> sha1_xor_base_key(std::span<const std::byte> qa_digest);

void sha1_xor(std::span<const std::byte> base_key, std::uint64_t data_relative_offset, std::span<const std::byte> in, std::span<std::byte> out);

} // namespace ps3hdd::crypto