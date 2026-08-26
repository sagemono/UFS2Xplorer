#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace ps3hdd::crypto {

inline void bswap16_in_place(std::span<std::byte> data) noexcept {
    const std::size_t n = data.size() & ~static_cast<std::size_t>(1);
    for (std::size_t i = 0; i < n; i += 2) {
        std::byte t = data[i];
        data[i] = data[i + 1];
        data[i + 1] = t;
    }
}

} // namespace ps3hdd::crypto