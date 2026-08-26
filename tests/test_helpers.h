#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace th {

inline std::vector<std::byte> rng(std::size_t n, std::uint64_t seed) {
    std::vector<std::byte> v(n);
    std::uint64_t x = seed ? seed : 0x9e3779b97f4a7c15ull;
    for (std::size_t i = 0; i < n; ++i) {
        x ^= x >> 12; x ^= x << 25; x ^= x >> 27;
        v[i] = static_cast<std::byte>((x * 0x2545f4914f6cdd1dull) >> 56);
    }
    return v;
}

inline std::span<const std::byte> cspan(const std::vector<std::byte>& v) { return {v.data(), v.size()}; }
inline std::span<std::byte> mspan(std::vector<std::byte>& v) { return {v.data(), v.size()}; }

} // namespace th