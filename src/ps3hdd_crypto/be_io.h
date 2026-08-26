#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

namespace ps3hdd {

inline void write_be_u16(std::byte* p, std::uint16_t v) noexcept {
    p[0] = static_cast<std::byte>((v >> 8) & 0xff);
    p[1] = static_cast<std::byte>(v & 0xff);
}

inline void write_be_u32(std::byte* p, std::uint32_t v) noexcept {
    p[0] = static_cast<std::byte>((v >> 24) & 0xff);
    p[1] = static_cast<std::byte>((v >> 16) & 0xff);
    p[2] = static_cast<std::byte>((v >> 8) & 0xff);
    p[3] = static_cast<std::byte>(v & 0xff);
}

inline void write_be_u64(std::byte* p, std::uint64_t v) noexcept {
    write_be_u32(p + 0, static_cast<std::uint32_t>(v >> 32));
    write_be_u32(p + 4, static_cast<std::uint32_t>(v & 0xffffffffu));
}

inline std::uint16_t read_be_u16(const std::byte* p) noexcept {
    return static_cast<std::uint16_t>((std::to_integer<std::uint16_t>(p[0]) << 8) | std::to_integer<std::uint16_t>(p[1]));
}

inline std::uint32_t read_be_u32(const std::byte* p) noexcept {
    return (std::to_integer<std::uint32_t>(p[0]) << 24) | (std::to_integer<std::uint32_t>(p[1]) << 16) | (std::to_integer<std::uint32_t>(p[2]) << 8) | std::to_integer<std::uint32_t>(p[3]);
}

inline std::uint64_t read_be_u64(const std::byte* p) noexcept {
    std::uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v = (v << 8) | std::to_integer<std::uint8_t>(p[i]);
    return v;
}

// bounds checked span variants
// return 0 when the read would run off the end
inline std::uint32_t read_be_u32(std::span<const std::byte> b, std::size_t off) noexcept {
    return off + 4 <= b.size() ? read_be_u32(b.data() + off) : 0;
}

inline std::uint64_t read_be_u64(std::span<const std::byte> b, std::size_t off) noexcept {
    return off + 8 <= b.size() ? read_be_u64(b.data() + off) : 0;
}

// nul terminated string at off, bounded by max_len (or the span end...)
inline std::string read_cstr(std::span<const std::byte> b, std::size_t off, std::size_t max_len = SIZE_MAX) {
    std::string s;
    if (off >= b.size()) return s;
    const std::size_t end = off + std::min(max_len, b.size() - off);
    for (std::size_t i = off; i < end; ++i) {
        const auto c = std::to_integer<char>(b[i]);
        if (c == '\0') break;
        s.push_back(c);
    }
    return s;
}

} // namespace ps3hdd