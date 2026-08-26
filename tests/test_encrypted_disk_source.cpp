#include "memory_disk_source.h"
#include "test_helpers.h"

#include <ps3hdd_crypto/ps3_key_derivation.h>
#include <ps3hdd_cryptodisk/encrypted_disk_source.h>

#include <catch2/catch_test_macros.hpp>

#include <memory>

using namespace ps3hdd;

TEST_CASE("ATA key derivation is deterministic and key dependent", "[cryptodisk][derive]") {
    auto eid_a = th::rng(48, 1);
    auto eid_b = th::rng(48, 2);

    const auto k1 = crypto::derive_ata_xts_keys(th::cspan(eid_a));
    const auto k2 = crypto::derive_ata_xts_keys(th::cspan(eid_a));
    const auto k3 = crypto::derive_ata_xts_keys(th::cspan(eid_b));

    REQUIRE(k1.data_key == k2.data_key);
    REQUIRE(k1.tweak_key == k2.tweak_key);
    REQUIRE(k1.data_key != k1.tweak_key);
    REQUIRE(k1.data_key != k3.data_key);
}

TEST_CASE("derivation rejects wrong size EID keys", "[cryptodisk][derive]") {
    auto bad = th::rng(32, 3);
    REQUIRE_THROWS(crypto::derive_ata_xts_keys(th::cspan(bad)));
}

TEST_CASE("encrypted_disk_source round-trips through the inner store", "[cryptodisk]") {
    const auto dk = th::rng(16, 10);
    const auto tk = th::rng(16, 11);

    auto inner = std::make_shared<th::memory_disk_source>(64 * 1024);
    auto* inner_raw = inner.get();
    cryptodisk::encrypted_disk_source dec(inner, th::cspan(dk), th::cspan(tk), /*bswap16=*/true);

    const auto plaintext = th::rng(8 * 512, 12);
    dec.write_bytes(0, th::cspan(plaintext));

    auto back = dec.read_bytes(0, plaintext.size());
    REQUIRE(back == plaintext);

    auto stored = inner_raw->read_bytes(0, plaintext.size());
    REQUIRE(stored != plaintext);
}

TEST_CASE("encrypted_disk_source decrypts a byte range across a sector boundary", "[cryptodisk]") {
    const auto dk = th::rng(16, 20);
    const auto tk = th::rng(16, 21);

    auto inner = std::make_shared<th::memory_disk_source>(64 * 1024);
    cryptodisk::encrypted_disk_source dec(inner, th::cspan(dk), th::cspan(tk), true);

    const auto plaintext = th::rng(4 * 512, 22);
    dec.write_bytes(0, th::cspan(plaintext));

    auto window = dec.read_bytes(600, 500);
    REQUIRE(window.size() == 500);
    REQUIRE(std::equal(window.begin(), window.end(), plaintext.begin() + 600));
}