#include <ps3hdd_license/rap.h>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <span>

using namespace ps3hdd;

namespace {
std::array<std::byte, 16> bytes_of(std::uint8_t seed) {
    std::array<std::byte, 16> a{};
    for (int i = 0; i < 16; ++i) a[i] = static_cast<std::byte>((i * 37 + seed) & 0xFF);
    return a;
}
} // namespace

TEST_CASE("rap_to_klicensee is deterministic and 16 bytes", "[license][rap]") {
    const auto rap = bytes_of(3);
    const auto k1 = license::rap_to_klicensee(rap);
    const auto k2 = license::rap_to_klicensee(rap);
    REQUIRE(k1 == k2);
    REQUIRE(k1.size() == 16);
}

TEST_CASE("different RAPs produce different klicensees", "[license][rap]") {
    REQUIRE(license::rap_to_klicensee(bytes_of(1)) != license::rap_to_klicensee(bytes_of(2)));
}

TEST_CASE("rap_to_klicensee rejects a wrong-size RAP", "[license][rap]") {
    std::array<std::byte, 15> short_rap{};
    REQUIRE_THROWS(license::rap_to_klicensee(std::span<const std::byte>(short_rap)));
}