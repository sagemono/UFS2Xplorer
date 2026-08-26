#include "aes_cbc_192.h"

#include "openssl_error.h"

#include <openssl/evp.h>

#include <memory>
#include <stdexcept>

namespace ps3hdd::crypto {

namespace {
using ctx_ptr = std::unique_ptr<EVP_CIPHER_CTX, decltype(&EVP_CIPHER_CTX_free)>;
}

aes_cbc_192::aes_cbc_192(std::span<const std::byte> key) {
    if (key.size() != key_size)
        throw std::invalid_argument("aes_cbc_192: key must be 24 bytes");
    std::copy(key.begin(), key.end(), key_.begin());
}

void aes_cbc_192::process(std::span<const std::byte> in, std::span<std::byte> out, bool encrypt) const {
    if (in.size() != out.size())
        throw std::invalid_argument("aes_cbc_192: input/output size mismatch");
    if (in.size() % sector_size != 0)
        throw std::invalid_argument("aes_cbc_192: length must be a multiple of 512");

    ctx_ptr ctx(EVP_CIPHER_CTX_new(), &EVP_CIPHER_CTX_free);
    if (!ctx) throw_openssl("EVP_CIPHER_CTX_new");

    const EVP_CIPHER* cipher = EVP_aes_192_cbc();
    const std::array<unsigned char, 16> zero_iv{};
    const std::size_t sectors = in.size() / sector_size;

    for (std::size_t s = 0; s < sectors; ++s) {
        if (EVP_CipherInit_ex(ctx.get(), cipher, nullptr, reinterpret_cast<const unsigned char*>(key_.data()), zero_iv.data(), encrypt ? 1 : 0) != 1)
            throw_openssl("EVP_CipherInit_ex (cbc)");
        EVP_CIPHER_CTX_set_padding(ctx.get(), 0);

        int out_len = 0;
        const auto* src = reinterpret_cast<const unsigned char*>(in.data() + s * sector_size);
        auto* dst = reinterpret_cast<unsigned char*>(out.data() + s * sector_size);
        if (EVP_CipherUpdate(ctx.get(), dst, &out_len, src, static_cast<int>(sector_size)) != 1)
            throw_openssl("EVP_CipherUpdate (cbc)");
        int final_len = 0;
        if (EVP_CipherFinal_ex(ctx.get(), dst + out_len, &final_len) != 1)
            throw_openssl("EVP_CipherFinal_ex (cbc)");
        if (out_len + final_len != static_cast<int>(sector_size))
            throw crypto_error("aes_cbc_192: short CBC output");
    }
}

void aes_cbc_192::encrypt_sectors(std::span<const std::byte> plaintext, std::span<std::byte> ciphertext) const {
    process(plaintext, ciphertext, true);
}

void aes_cbc_192::decrypt_sectors(std::span<const std::byte> ciphertext, std::span<std::byte> plaintext) const {
    process(ciphertext, plaintext, false);
}

} // namespace ps3hdd::crypto