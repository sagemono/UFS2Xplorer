#include "aes_xts_128.h"

#include "openssl_error.h"

#include <openssl/evp.h>

#include <memory>
#include <stdexcept>

namespace ps3hdd::crypto {

namespace {
using ctx_ptr = std::unique_ptr<EVP_CIPHER_CTX, decltype(&EVP_CIPHER_CTX_free)>;
}

aes_xts_128::aes_xts_128(std::span<const std::byte> data_key, std::span<const std::byte> tweak_key) {
    if (data_key.size() != key_size || tweak_key.size() != key_size)
        throw std::invalid_argument("aes_xts_128: keys must be 16 bytes");
    std::copy(data_key.begin(), data_key.end(), key_.begin());
    std::copy(tweak_key.begin(), tweak_key.end(), key_.begin() + key_size);
}

void aes_xts_128::process(std::span<const std::byte> in, std::span<std::byte> out, std::uint64_t start_sector, bool encrypt) const {
    if (in.size() != out.size())
        throw std::invalid_argument("aes_xts_128: input/output size mismatch");
    if (in.size() % sector_size != 0)
        throw std::invalid_argument("aes_xts_128: length must be a multiple of 512");

    ctx_ptr ctx(EVP_CIPHER_CTX_new(), &EVP_CIPHER_CTX_free);
    if (!ctx) throw_openssl("EVP_CIPHER_CTX_new");

    const EVP_CIPHER* cipher = EVP_aes_128_xts();
    const std::size_t sectors = in.size() / sector_size;

    for (std::size_t s = 0; s < sectors; ++s) {
        const std::uint64_t sector = start_sector + s;

        // tweak/iv = LE 128 bit sector number
        std::array<unsigned char, 16> iv{};
        for (int i = 0; i < 8; ++i)
            iv[i] = static_cast<unsigned char>((sector >> (8 * i)) & 0xff);

        if (EVP_CipherInit_ex(ctx.get(), cipher, nullptr, reinterpret_cast<const unsigned char*>(key_.data()), iv.data(), encrypt ? 1 : 0) != 1)
            throw_openssl("EVP_CipherInit_ex (xts)");

        int out_len = 0;
        const auto* src = reinterpret_cast<const unsigned char*>(in.data() + s * sector_size);
        auto* dst = reinterpret_cast<unsigned char*>(out.data() + s * sector_size);
        if (EVP_CipherUpdate(ctx.get(), dst, &out_len, src, static_cast<int>(sector_size)) != 1)
            throw_openssl("EVP_CipherUpdate (xts)");
        if (out_len != static_cast<int>(sector_size))
            throw crypto_error("aes_xts_128: short XTS output");
    }
}

void aes_xts_128::encrypt_sectors(std::span<const std::byte> plaintext, std::span<std::byte> ciphertext,std::uint64_t start_sector) const {
    process(plaintext, ciphertext, start_sector, true);
}

void aes_xts_128::decrypt_sectors(std::span<const std::byte> ciphertext, std::span<std::byte> plaintext, std::uint64_t start_sector) const {
    process(ciphertext, plaintext, start_sector, false);
}

} // namespace ps3hdd::crypto