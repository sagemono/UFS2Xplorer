#pragma once

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace ps3hdd::tools {

inline std::vector<std::byte> parse_hex(const std::string& s) {
    std::string h;
    for (char c : s) if (std::isxdigit(static_cast<unsigned char>(c))) h.push_back(c);
    std::vector<std::byte> out;
    for (std::size_t i = 0; i + 1 < h.size(); i += 2)
        out.push_back(static_cast<std::byte>(std::stoi(h.substr(i, 2), nullptr, 16)));
    return out;
}

inline std::vector<std::byte> read_file(const std::filesystem::path& p) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return {};
    f.seekg(0, std::ios::end);
    const auto n = static_cast<std::size_t>(f.tellg());
    f.seekg(0);
    std::vector<std::byte> b(n);
    f.read(reinterpret_cast<char*>(b.data()), static_cast<std::streamsize>(n));
    return b;
}

inline void hexdump(const std::vector<std::byte>& data, std::size_t count) {
    count = std::min(count, data.size());
    for (std::size_t i = 0; i < count; i += 16) {
        std::printf("  %04zx  ", i);
        for (std::size_t j = 0; j < 16; ++j) {
            if (i + j < count) std::printf("%02x ", std::to_integer<unsigned>(data[i + j]));
            else std::printf("   ");
        }
        std::printf(" ");
        for (std::size_t j = 0; j < 16 && i + j < count; ++j) {
            const int c = std::to_integer<int>(data[i + j]);
            std::printf("%c", (c >= 32 && c < 127) ? c : '.');
        }
        std::printf("\n");
    }
}

} // namespace ps3hdd::tools