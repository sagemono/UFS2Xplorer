#include "test_helpers.h"

#include <ps3hdd_crypto/sha1_xor.h>

#include <openssl/sha.h>
#include <catch2/catch_test_macros.hpp>

using namespace ps3hdd::crypto;

TEST_CASE("SHA1-XOR is symmetric", "[sha1xor]") {
    const auto qa = th::rng(16, 31);
    const auto base = sha1_xor_base_key(th::cspan(qa));

    const auto pt = th::rng(5000, 32);
    std::vector<std::byte> ct(pt.size()), rt(pt.size());
    sha1_xor({base.data(), base.size()}, 0, th::cspan(pt), th::mspan(ct));
    sha1_xor({base.data(), base.size()}, 0, th::cspan(ct), th::mspan(rt));
    REQUIRE(rt == pt);
}

TEST_CASE("SHA1-XOR is seekable at block boundaries", "[sha1xor]") {
    const auto qa = th::rng(16, 33);
    const auto base = sha1_xor_base_key(th::cspan(qa));

    const auto in = th::rng(4096, 34);
    std::vector<std::byte> whole(in.size());
    sha1_xor({base.data(), base.size()}, 0, th::cspan(in), th::mspan(whole));

    const std::size_t off = 2048; // block aligned
    std::span<const std::byte> in2(in.data() + off, in.size() - off);
    std::vector<std::byte> part(in.size() - off);
    sha1_xor({base.data(), base.size()}, off, in2, th::mspan(part));
    REQUIRE(std::equal(part.begin(), part.end(), whole.begin() + off));
}

TEST_CASE("SHA1-XOR keystream matches a direct SHA1 of the key buffer", "[sha1xor]") {
    const auto qa = th::rng(16, 35);
    const auto base = sha1_xor_base_key(th::cspan(qa));

    std::vector<std::byte> zero(16, std::byte{0}), out(16);
    sha1_xor({base.data(), base.size()}, 0, th::cspan(zero), th::mspan(out));

    unsigned char hash[SHA_DIGEST_LENGTH];
    SHA1(reinterpret_cast<const unsigned char*>(base.data()), base.size(), hash);
    for (int i = 0; i < 16; ++i)
        REQUIRE(std::to_integer<unsigned>(out[i]) == hash[i]);
}
