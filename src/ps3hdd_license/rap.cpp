#include "rap.h"

#include <openssl/evp.h>

#include <array>
#include <cstdint>
#include <stdexcept>

namespace ps3hdd::license {

namespace {

constexpr std::uint8_t kRapAesKey[16] = {0x86, 0x9F, 0x77, 0x45, 0xC1, 0x3F, 0xD8, 0x90,0xCC, 0xF2, 0x91, 0x88, 0xE3, 0xCC, 0x3E, 0xDF};
constexpr int kPbox[16] = {0x0C, 0x03, 0x06, 0x04, 0x01, 0x0B, 0x0F, 0x08,0x02, 0x07, 0x00, 0x05, 0x0A, 0x0E, 0x0D, 0x09};
constexpr std::uint8_t kXorE1[16] = {0xA9, 0x3E, 0x1F, 0xD6, 0x7C, 0x55, 0xA3, 0x29,0xB7, 0x5F, 0xDD, 0xA6, 0x2A, 0x95, 0xC7, 0xA5};
constexpr std::uint8_t kSubE2[16] = {0x67, 0xD4, 0x5D, 0xA3, 0x29, 0x6D, 0x00, 0x6A,0x4E, 0x7C, 0x53, 0x7B, 0xF5, 0x53, 0x8C, 0x74};

void aes128_ecb_decrypt(const std::uint8_t key[16], const std::uint8_t in[16], std::uint8_t out[16]) {
    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (!ctx) throw std::runtime_error("EVP_CIPHER_CTX_new failed");
    int len = 0;
    const bool ok = EVP_DecryptInit_ex(ctx, EVP_aes_128_ecb(), nullptr, key, nullptr) == 1 && EVP_CIPHER_CTX_set_padding(ctx, 0) == 1 && EVP_DecryptUpdate(ctx, out, &len, in, 16) == 1;
    EVP_CIPHER_CTX_free(ctx);
    if (!ok) throw std::runtime_error("AES-128-ECB decrypt failed");
}

} // namespace

std::array<std::byte, 16> rap_to_klicensee(std::span<const std::byte> rap) {
    if (rap.size() != 16) throw std::invalid_argument("RAP must be exactly 16 bytes");

    std::uint8_t k[16];
    {
        std::uint8_t in[16];
        for (int i = 0; i < 16; ++i) in[i] = std::to_integer<std::uint8_t>(rap[i]);
        aes128_ecb_decrypt(kRapAesKey, in, k);
    }

    for (int round = 0; round < 5; ++round) {
        for (int i = 0; i < 16; ++i) k[i] ^= kXorE1[i];
        for (int i = 15; i >= 1; --i) k[kPbox[i]] ^= k[kPbox[i - 1]];
        int borrow = 0;
        for (int i = 0; i < 16; ++i) {
            const int p = kPbox[i];
            const int v = static_cast<int>(k[p]) - static_cast<int>(kSubE2[p]) - borrow;
            k[p] = static_cast<std::uint8_t>(v);
            borrow = v < 0 ? 1 : 0;
        }
    }

    std::array<std::byte, 16> klic{};
    for (int i = 0; i < 16; ++i) klic[i] = static_cast<std::byte>(k[i]);
    return klic;
}

} // namespace ps3hdd::license