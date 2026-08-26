#include "test_helpers.h"

#include <ps3hdd_crypto/aes_cbc_192.h>

#include <openssl/evp.h>
#include <catch2/catch_test_macros.hpp>

using namespace ps3hdd::crypto;

// oneshot EVP CBC-192 with zero IV, no padding, for a single sector
static std::vector<std::byte> ref_cbc192(std::span<const std::byte> in, std::span<const std::byte> key, bool encrypt) {
    std::vector<std::byte> out(in.size());
    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    const std::array<unsigned char, 16> iv{};
    int len = 0, fin = 0;
    EVP_CipherInit_ex(ctx, EVP_aes_192_cbc(), nullptr, reinterpret_cast<const unsigned char*>(key.data()), iv.data(), encrypt ? 1 : 0);
    EVP_CIPHER_CTX_set_padding(ctx, 0);
    EVP_CipherUpdate(ctx, reinterpret_cast<unsigned char*>(out.data()), &len, reinterpret_cast<const unsigned char*>(in.data()), static_cast<int>(in.size()));
    EVP_CipherFinal_ex(ctx, reinterpret_cast<unsigned char*>(out.data()) + len, &fin);
    EVP_CIPHER_CTX_free(ctx);
    return out;
}

TEST_CASE("CBC-192 per-sector round-trip", "[cbc]") {
    const auto key = th::rng(24, 21);
    aes_cbc_192 cipher(th::cspan(key));

    const auto pt = th::rng(8 * 512, 22);
    std::vector<std::byte> ct(pt.size()), rt(pt.size());
    cipher.encrypt_sectors(th::cspan(pt), th::mspan(ct));
    cipher.decrypt_sectors(th::cspan(ct), th::mspan(rt));
    REQUIRE(rt == pt);
}

TEST_CASE("CBC-192 first sector matches a one-shot EVP reference", "[cbc]") {
    const auto key = th::rng(24, 23);
    aes_cbc_192 cipher(th::cspan(key));

    const auto pt = th::rng(512, 24);
    std::vector<std::byte> ct(512);
    cipher.encrypt_sectors(th::cspan(pt), th::mspan(ct));
    const auto ref = ref_cbc192(th::cspan(pt), th::cspan(key), true);
    REQUIRE(ct == ref);
}

TEST_CASE("CBC-192 resets IV every sector (independent sectors)", "[cbc]") {
    const auto key = th::rng(24, 25);
    aes_cbc_192 cipher(th::cspan(key));
    auto one = th::rng(512, 26);
    std::vector<std::byte> two(1024);
    std::copy(one.begin(), one.end(), two.begin());
    std::copy(one.begin(), one.end(), two.begin() + 512);
    std::vector<std::byte> ct(1024);
    cipher.encrypt_sectors(th::cspan(two), th::mspan(ct));
    REQUIRE(std::equal(ct.begin(), ct.begin() + 512, ct.begin() + 512));
}