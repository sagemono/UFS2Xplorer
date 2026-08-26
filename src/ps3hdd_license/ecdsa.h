#pragma once

#include <cstdint>

namespace ps3hdd::license {

void ps3_ecdsa_sign(const std::uint8_t hash[20], std::uint8_t r[21], std::uint8_t s[21]);
bool ps3_ecdsa_verify(const std::uint8_t hash[20], const std::uint8_t r[21], const std::uint8_t s[21]);
bool ps3_pkg_ecdsa_verify(const std::uint8_t hash[20], const std::uint8_t r[21], const std::uint8_t s[21]);

} // namespace ps3hdd::license