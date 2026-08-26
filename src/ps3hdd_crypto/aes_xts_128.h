#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace ps3hdd::crypto {

class aes_xts_128 {
public:
    static constexpr std::size_t sector_size = 512;
    static constexpr std::size_t key_size = 16;

    aes_xts_128(std::span<const std::byte> data_key, std::span<const std::byte> tweak_key);

    void encrypt_sectors(std::span<const std::byte> plaintext, std::span<std::byte> ciphertext, std::uint64_t start_sector) const;
    void decrypt_sectors(std::span<const std::byte> ciphertext, std::span<std::byte> plaintext, std::uint64_t start_sector) const;

private:
    std::array<std::byte, 2 * key_size> key_{};

    void process(std::span<const std::byte> in, std::span<std::byte> out, std::uint64_t start_sector, bool encrypt) const;
};

} // namespace ps3hdd::crypto
