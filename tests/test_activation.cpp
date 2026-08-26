#include <ps3hdd_license/activation.h>
#include <ps3hdd_license/ecdsa.h>
#include <ps3hdd_license/rap.h>

#include <catch2/catch_test_macros.hpp>

#include <openssl/evp.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <vector>

using namespace ps3hdd;

namespace {
std::array<std::byte, 16> fill16(std::uint8_t seed) {
    std::array<std::byte, 16> a{};
    for (int i = 0; i < 16; ++i) a[i] = static_cast<std::byte>((i * 53 + seed) & 0xFF);
    return a;
}
} // namespace

TEST_CASE("act.dat + rif roundtrip: console ladder recovers the klicensee", "[license][activation]") {
    const auto idps = fill16(0x11);
    const auto klic = fill16(0x22);
    const std::string cid = "EP9000-NPEA00241_00-GLITTLEBIG000001";
    const std::uint64_t account_id = 0;

    const auto act = license::build_activation(idps, cid, klic, account_id);
    REQUIRE(act.act_dat.size() == 0x1038);
    REQUIRE(act.rif.size() == 0x98);

    const auto recovered = license::rif_recover_klicensee(idps, act.act_dat, act.rif);
    REQUIRE(recovered == klic);
}

TEST_CASE("build_rif reuses an existing act.dat and stays consistent", "[license][activation][rifonly]") {
    const auto idps = fill16(0x11);
    const auto existing = license::build_activation(idps, "UP0001-EXIST0001_00-0000000000000000", fill16(0xAB), 0x1234);
    const auto klic2 = fill16(0x77);
    const auto rif = license::build_rif(idps, "EP9000-NPEA00453_00-RATCHET000000001", klic2, existing.act_dat);

    REQUIRE(license::rif_recover_klicensee(idps, existing.act_dat, rif) == klic2);
    for (int i = 0; i < 8; ++i) REQUIRE(rif[0x08 + i] == existing.act_dat[0x08 + i]);
}

TEST_CASE("a different IDPS cannot recover the klicensee", "[license][activation]") {
    const auto idps = fill16(0x11);
    const auto other = fill16(0x99);
    const auto klic = fill16(0x22);
    const auto act = license::build_activation(idps, "UP0001-TEST00001_00-0000000000000000", klic, 0);
    REQUIRE(license::rif_recover_klicensee(other, act.act_dat, act.rif) != klic);
}

TEST_CASE("the generated rif carries a valid NPDRM ECDSA signature", "[license][activation][ecdsa]") {
    const auto idps = fill16(0x11);
    const auto klic = fill16(0x22);
    const auto act = license::build_activation(idps, "EP9000-NPEA00241_00-GLITTLEBIG000001", klic, 0);

    auto to_bytes = [](std::span<const std::byte> s) {
        std::vector<std::uint8_t> v(s.size());
        for (std::size_t i = 0; i < s.size(); ++i) v[i] = std::to_integer<std::uint8_t>(s[i]);
        return v;
    };
    const auto actb = to_bytes(act.act_dat);
    const auto rifb = to_bytes(act.rif);

    auto verify_region = [](const std::vector<std::uint8_t>& buf, std::size_t hash_len, std::size_t r_off, std::size_t s_off) {
        std::uint8_t hash[20];
        unsigned int n = 0;
        REQUIRE(EVP_Digest(buf.data(), hash_len, hash, &n, EVP_sha1(), nullptr) == 1);
        std::uint8_t R[21]{}, S[21]{};
        std::memcpy(R + 1, buf.data() + r_off, 20);
        std::memcpy(S + 1, buf.data() + s_off, 20);
        return license::ps3_ecdsa_verify(hash, R, S);
    };
    (void)actb; // act.dat is unsigned by design
    REQUIRE(verify_region(rifb, 0x70, 0x70, 0x84));
}