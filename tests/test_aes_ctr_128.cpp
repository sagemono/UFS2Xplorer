#include "test_helpers.h"
#include "test_ref.h"

#include <ps3hdd_crypto/aes_ctr_128.h>

#include <catch2/catch_test_macros.hpp>

using namespace ps3hdd::crypto;

TEST_CASE("CTR matches an independent reference", "[ctr]") {
    const auto key = th::rng(16, 11);
    const auto riv = th::rng(16, 12);
    aes_ctr_128 cipher(th::cspan(key));

    for (std::size_t n : {std::size_t{16}, std::size_t{64}, std::size_t{1000}, std::size_t{65536}}) {
        for (std::uint64_t sb : {std::uint64_t{0}, std::uint64_t{1}, std::uint64_t{123456}}) {
            const auto in = th::rng(n, n + sb);
            std::vector<std::byte> out(n);
            cipher.process(th::cspan(in), th::mspan(out), th::cspan(riv), sb);
            const auto ref_out = ref::ctr(th::cspan(in), th::cspan(key), th::cspan(riv), sb);
            REQUIRE(out == ref_out);
        }
    }
}

TEST_CASE("CTR is seekable at block boundaries", "[ctr]") {
    const auto key = th::rng(16, 13);
    const auto riv = th::rng(16, 14);
    aes_ctr_128 cipher(th::cspan(key));

    const auto in = th::rng(4096, 77);
    std::vector<std::byte> whole(4096);
    cipher.process(th::cspan(in), th::mspan(whole), th::cspan(riv), 0);

    const std::size_t half = 2048;
    std::span<const std::byte> in2(in.data() + half, half);
    std::vector<std::byte> part(half);
    cipher.process(in2, th::mspan(part), th::cspan(riv), half / 16);
    REQUIRE(std::equal(part.begin(), part.end(), whole.begin() + half));
}

TEST_CASE("counter_add carries across bytes", "[ctr]") {
    std::vector<std::byte> riv(16, std::byte{0});
    riv[15] = std::byte{0xff};
    const auto c = counter_add(th::cspan(riv), 1);
    REQUIRE(c[15] == std::byte{0x00});
    REQUIRE(c[14] == std::byte{0x01});
}