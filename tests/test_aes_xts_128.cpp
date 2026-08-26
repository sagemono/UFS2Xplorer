#include "test_helpers.h"
#include "test_ref.h"

#include <ps3hdd_crypto/aes_xts_128.h>

#include <catch2/catch_test_macros.hpp>

using namespace ps3hdd::crypto;

TEST_CASE("XTS matches an independent reference across sizes and offsets", "[xts]") {
    const auto dk = th::rng(16, 1);
    const auto tk = th::rng(16, 2);
    aes_xts_128 cipher(th::cspan(dk), th::cspan(tk));

    for (std::size_t sectors : {std::size_t{1}, std::size_t{2}, std::size_t{7}, std::size_t{64}, std::size_t{129}}) {
        for (std::uint64_t start : {std::uint64_t{0}, std::uint64_t{1}, std::uint64_t{0x12345678ull}}) {
            const auto pt = th::rng(sectors * 512, sectors * 100 + start);
            std::vector<std::byte> ct(pt.size());

            cipher.encrypt_sectors(th::cspan(pt), th::mspan(ct), start);
            const auto ref_ct = ref::xts(th::cspan(pt), start, th::cspan(dk), th::cspan(tk), true);
            REQUIRE(ct == ref_ct);

            std::vector<std::byte> rt(pt.size());
            cipher.decrypt_sectors(th::cspan(ct), th::mspan(rt), start);
            REQUIRE(rt == pt);
        }
    }
}

TEST_CASE("XTS decrypt matches reference", "[xts]") {
    const auto dk = th::rng(16, 3);
    const auto tk = th::rng(16, 4);
    aes_xts_128 cipher(th::cspan(dk), th::cspan(tk));

    const auto ct = th::rng(200 * 512, 999);
    std::vector<std::byte> pt(ct.size());
    cipher.decrypt_sectors(th::cspan(ct), th::mspan(pt), 0x9abcdef0ull);
    const auto ref_pt = ref::xts(th::cspan(ct), 0x9abcdef0ull, th::cspan(dk), th::cspan(tk), false);
    REQUIRE(pt == ref_pt);
}

TEST_CASE("XTS works in place", "[xts]") {
    const auto dk = th::rng(16, 5);
    const auto tk = th::rng(16, 6);
    aes_xts_128 cipher(th::cspan(dk), th::cspan(tk));

    auto buf = th::rng(4 * 512, 42);
    const auto orig = buf;
    cipher.encrypt_sectors(th::cspan(buf), th::mspan(buf), 7);
    REQUIRE(buf != orig);
    cipher.decrypt_sectors(th::cspan(buf), th::mspan(buf), 7);
    REQUIRE(buf == orig);
}

TEST_CASE("XTS rejects bad sizes and keys", "[xts]") {
    const auto dk = th::rng(16, 7);
    const auto tk = th::rng(16, 8);
    const auto bad = th::rng(15, 9);
    REQUIRE_THROWS(aes_xts_128(th::cspan(dk), th::cspan(bad)));

    aes_xts_128 cipher(th::cspan(dk), th::cspan(tk));
    auto in = th::rng(500, 10); // not a multiple of 512!
    std::vector<std::byte> out(500);
    REQUIRE_THROWS(cipher.encrypt_sectors(th::cspan(in), th::mspan(out), 0));
}