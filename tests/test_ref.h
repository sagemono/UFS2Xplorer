#pragma once

#include <openssl/evp.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <vector>

namespace ref {

inline std::array<std::byte, 16> ecb_block(std::span<const std::byte> key16, std::span<const std::byte, 16> in, bool encrypt) {
    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    std::array<std::byte, 16> out{};
    int len = 0;
    EVP_CipherInit_ex(ctx, EVP_aes_128_ecb(), nullptr, reinterpret_cast<const unsigned char*>(key16.data()), nullptr, encrypt ? 1 : 0);
    EVP_CIPHER_CTX_set_padding(ctx, 0);
    EVP_CipherUpdate(ctx, reinterpret_cast<unsigned char*>(out.data()), &len, reinterpret_cast<const unsigned char*>(in.data()), 16);
    EVP_CIPHER_CTX_free(ctx);
    return out;
}

// GF(2^128) multiply by alpha, le, reduction poly 0x87.
inline void gf_mul_alpha(std::array<std::byte, 16>& t) {
    unsigned carry = 0;
    for (int i = 0; i < 16; ++i) {
        unsigned b = std::to_integer<unsigned>(t[i]);
        unsigned next = (b >> 7) & 1;
        t[i] = static_cast<std::byte>(((b << 1) | carry) & 0xff);
        carry = next;
    }
    if (carry) t[0] ^= std::byte{0x87};
}

// ref xts over 512 byte sectors, distinct keys, LE sector tweak
inline std::vector<std::byte> xts(std::span<const std::byte> in, std::uint64_t start_sector, std::span<const std::byte> data_key, std::span<const std::byte> tweak_key, bool encrypt) {
    std::vector<std::byte> out(in.size());
    const std::size_t sectors = in.size() / 512;
    for (std::size_t s = 0; s < sectors; ++s) {
        const std::uint64_t sector = start_sector + s;
        std::array<std::byte, 16> tw{};
        for (int i = 0; i < 8; ++i) tw[i] = static_cast<std::byte>((sector >> (8 * i)) & 0xff);
        tw = ecb_block(tweak_key, tw, true);
        for (int j = 0; j < 32; ++j) {
            std::array<std::byte, 16> blk{};
            for (int k = 0; k < 16; ++k) blk[k] = in[s * 512 + j * 16 + k] ^ tw[k];
            blk = ecb_block(data_key, blk, encrypt);
            for (int k = 0; k < 16; ++k) out[s * 512 + j * 16 + k] = blk[k] ^ tw[k];
            gf_mul_alpha(tw);
        }
    }
    return out;
}

inline std::array<std::byte, 16> counter_add(std::span<const std::byte> riv, std::uint64_t blocks) {
    std::array<std::byte, 16> c{};
    for (int i = 0; i < 16; ++i) c[i] = riv[i];
    std::uint64_t carry = blocks;
    for (int i = 15; i >= 0 && carry; --i) {
        std::uint64_t sum = std::to_integer<std::uint64_t>(c[i]) + (carry & 0xff);
        c[i] = static_cast<std::byte>(sum & 0xff);
        carry = (carry >> 8) + (sum >> 8);
    }
    return c;
}

// ref 128 ctr keystream XOR
inline std::vector<std::byte> ctr(std::span<const std::byte> in, std::span<const std::byte> key, std::span<const std::byte> riv, std::uint64_t start_block) {
    std::vector<std::byte> out(in.size());
    for (std::size_t i = 0; i < in.size(); ++i) {
        const std::uint64_t blk = start_block + i / 16;
        const std::array<std::byte, 16> counter = counter_add(riv, blk);
        const std::array<std::byte, 16> ks = ecb_block(key, counter, true);
        out[i] = in[i] ^ ks[i % 16];
    }
    return out;
}

} // namespace ref