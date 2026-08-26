#include "ps3_key_derivation.h"

#include "openssl_error.h"

#include <openssl/evp.h>

#include <array>
#include <memory>
#include <stdexcept>

namespace ps3hdd::crypto {

namespace {
using ctx_ptr = std::unique_ptr<EVP_CIPHER_CTX, decltype(&EVP_CIPHER_CTX_free)>;

// universal ATA seeds (same across fat nand, fat nor, slim)
constexpr std::array<unsigned char, 32> kAtaDataSeed = {
    0xD9,0x2D,0x65,0xDB,0x05,0x7D,0x49,0xE1,0xA6,0x6F,0x22,0x74,0xB8,0xBA,0xC5,0x08,
    0x83,0x84,0x4E,0xD7,0x56,0xCA,0x79,0x51,0x63,0x62,0xEA,0x8A,0xDA,0xC6,0x03,0x26
};

constexpr std::array<unsigned char, 32> kAtaTweakSeed = {
    0xC3,0xB3,0xB5,0xAA,0xCC,0x74,0xCD,0x6A,0x48,0xEF,0xAB,0xF4,0x4D,0xCD,0xF1,0x6E,
    0x37,0x9F,0x55,0xF5,0x77,0x7D,0x09,0xFB,0xEE,0xDE,0x07,0x05,0x8E,0x94,0xBE,0x08
};

// AES-256-CBC encrypt seed under key/iv, no padding
std::array<std::byte, 32> aes256_cbc_encrypt(std::span<const unsigned char> seed, const unsigned char* key, const unsigned char* iv) {
    ctx_ptr ctx(EVP_CIPHER_CTX_new(), &EVP_CIPHER_CTX_free);
    if (!ctx) throw_openssl("EVP_CIPHER_CTX_new");
    if (EVP_EncryptInit_ex(ctx.get(), EVP_aes_256_cbc(), nullptr, key, iv) != 1)
        throw_openssl("EVP_EncryptInit_ex (aes-256-cbc)");
    EVP_CIPHER_CTX_set_padding(ctx.get(), 0);

    std::array<std::byte, 32> out{};
    int len = 0;
    if (EVP_EncryptUpdate(ctx.get(), reinterpret_cast<unsigned char*>(out.data()), &len, seed.data(), static_cast<int>(seed.size())) != 1)
        throw_openssl("EVP_EncryptUpdate (aes-256-cbc)");
    int final_len = 0;
    if (EVP_EncryptFinal_ex(ctx.get(), reinterpret_cast<unsigned char*>(out.data()) + len, &final_len) != 1)
        throw_openssl("EVP_EncryptFinal_ex (aes-256-cbc)");
    return out;
}
} // namespace

ata_xts_keys derive_ata_xts_keys(std::span<const std::byte> eid_root_key) {
    if (eid_root_key.size() != 48)
        throw std::invalid_argument("EID Root Key must be 48 bytes");

    const auto* key = reinterpret_cast<const unsigned char*>(eid_root_key.data());
    const unsigned char* erk_key = key; // bytes 0..32
    const unsigned char* erk_iv = key + 32; // bytes 32..48

    const auto data = aes256_cbc_encrypt(kAtaDataSeed, erk_key, erk_iv);
    const auto tweak = aes256_cbc_encrypt(kAtaTweakSeed, erk_key, erk_iv);

    ata_xts_keys keys;
    std::copy_n(data.begin(), 16, keys.data_key.begin());
    std::copy_n(tweak.begin(), 16, keys.tweak_key.begin());
    return keys;
}

std::array<std::byte, 24> derive_ata_cbc_key(std::span<const std::byte> eid_root_key) {
    if (eid_root_key.size() != 48)
        throw std::invalid_argument("EID Root Key must be 48 bytes");

    const auto* key = reinterpret_cast<const unsigned char*>(eid_root_key.data());
    const auto data = aes256_cbc_encrypt(kAtaDataSeed, key, key + 32);

    std::array<std::byte, 24> out{};
    std::copy_n(data.begin(), 24, out.begin());
    return out;
}

} // namespace ps3hdd::crypto