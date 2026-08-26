#pragma once

#include <array>
#include <cstddef>
#include <span>

namespace ps3hdd::crypto {

class aes_cbc_192 {
public:
    static constexpr std::size_t sector_size = 512;
    static constexpr std::size_t key_size = 24;

    explicit aes_cbc_192(std::span<const std::byte> key);

    void encrypt_sectors(std::span<const std::byte> plaintext, std::span<std::byte> ciphertext) const;
    void decrypt_sectors(std::span<const std::byte> ciphertext, std::span<std::byte> plaintext) const;

private:
    std::array<std::byte, key_size> key_{};
    void process(std::span<const std::byte> in, std::span<std::byte> out, bool encrypt) const;
};

} // namespace ps3hdd::crypto