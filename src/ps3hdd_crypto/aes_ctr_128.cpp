#include "aes_ctr_128.h"

#include "openssl_error.h"

#include <openssl/evp.h>

#include <memory>
#include <stdexcept>

namespace ps3hdd::crypto {

namespace {
using ctx_ptr = std::unique_ptr<EVP_CIPHER_CTX, decltype(&EVP_CIPHER_CTX_free)>;
}

std::array<std::byte, 16> counter_add(std::span<const std::byte> riv, std::uint64_t blocks) {
    if (riv.size() != 16)
        throw std::invalid_argument("counter_add: riv must be 16 bytes");
    std::array<std::byte, 16> ctr{};
    std::copy(riv.begin(), riv.end(), ctr.begin());
    // BE 128 bit add of blocks into the low 64 bits with carry
    std::uint64_t carry = blocks;
    for (int i = 15; i >= 0 && carry != 0; --i) {
        const std::uint64_t sum = std::to_integer<std::uint64_t>(ctr[i]) + (carry & 0xff);
        ctr[i] = static_cast<std::byte>(sum & 0xff);
        carry = (carry >> 8) + (sum >> 8);
    }
    return ctr;
}

aes_ctr_128::aes_ctr_128(std::span<const std::byte> key) {
    if (key.size() != key_size)
        throw std::invalid_argument("aes_ctr_128: key must be 16 bytes");
    std::copy(key.begin(), key.end(), key_.begin());
}

void aes_ctr_128::process(std::span<const std::byte> in, std::span<std::byte> out, std::span<const std::byte> riv, std::uint64_t start_block) const {
    if (in.size() != out.size())
        throw std::invalid_argument("aes_ctr_128: input/output size mismatch");

    const std::array<std::byte, 16> ctr = counter_add(riv, start_block);

    ctx_ptr ctx(EVP_CIPHER_CTX_new(), &EVP_CIPHER_CTX_free);
    if (!ctx) throw_openssl("EVP_CIPHER_CTX_new");

    if (EVP_DecryptInit_ex(ctx.get(), EVP_aes_128_ctr(), nullptr, reinterpret_cast<const unsigned char*>(key_.data()), reinterpret_cast<const unsigned char*>(ctr.data())) != 1)
        throw_openssl("EVP_DecryptInit_ex (ctr)");

    int out_len = 0;
    if (EVP_DecryptUpdate(ctx.get(), reinterpret_cast<unsigned char*>(out.data()), &out_len, reinterpret_cast<const unsigned char*>(in.data()), static_cast<int>(in.size())) != 1)
        throw_openssl("EVP_DecryptUpdate (ctr)");
}

} // namespace ps3hdd::crypto