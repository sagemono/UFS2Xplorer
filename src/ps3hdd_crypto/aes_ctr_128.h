#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace ps3hdd::crypto {

class aes_ctr_128 {
public:
    static constexpr std::size_t key_size = 16;
    static constexpr std::size_t block_size = 16;

    explicit aes_ctr_128(std::span<const std::byte> key);

    void process(std::span<const std::byte> in, std::span<std::byte> out, std::span<const std::byte> riv, std::uint64_t start_block) const;

private:
    std::array<std::byte, key_size> key_{};
};

std::array<std::byte, 16> counter_add(std::span<const std::byte> riv, std::uint64_t blocks);

} // namespace ps3hdd::crypto