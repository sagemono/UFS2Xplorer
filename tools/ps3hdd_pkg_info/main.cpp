#include <ps3hdd_disk/disk_source.h>
#include <ps3hdd_pkg/ps3_pkg_reader.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <string>
#include <vector>

using namespace ps3hdd;

namespace {

std::string human(std::uint64_t bytes) { return disk::format_size(bytes); }

std::string sfo_value(const std::vector<std::byte>& d, const std::string& want) {
    auto b = [&](std::size_t o) { return o < d.size() ? std::to_integer<unsigned>(d[o]) : 0u; };
    auto u16 = [&](std::size_t o) { return b(o) | (b(o + 1) << 8); };
    auto u32 = [&](std::size_t o) { return u16(o) | (u16(o + 2) << 16); };
    if (d.size() < 0x14 || b(0) != 0x00 || b(1) != 0x50 || b(2) != 0x53 || b(3) != 0x46) return {};
    const unsigned key_start = u32(0x08), data_start = u32(0x0C), n = u32(0x10);
    for (unsigned i = 0; i < n; ++i) {
        const std::size_t base = 0x14 + static_cast<std::size_t>(i) * 16;
        if (base + 16 > d.size()) break;
        const unsigned key_off = u16(base), fmt = u16(base + 2), len = u32(base + 4), data_off = u32(base + 0x0C);
        std::string key;
        for (std::size_t k = key_start + key_off; k < d.size() && b(k); ++k) key.push_back(static_cast<char>(b(k)));
        if (key != want) continue;
        std::string s;
        const std::size_t dp = data_start + data_off;
        if (fmt == 0x0404) return std::to_string(u32(dp));
        for (std::size_t k = 0; k < len && dp + k < d.size() && b(dp + k); ++k) s.push_back(static_cast<char>(b(dp + k)));
        return s;
    }
    return {};
}

std::string pkg_sfo_value(pkg::ps3_pkg_reader& pkg, const std::string& want) {
    for (const auto& e : pkg.entries()) {
        if (e.is_directory) continue;
        if (e.name == "PARAM.SFO" || (e.name.size() > 10 && e.name.substr(e.name.size() - 10) == "/PARAM.SFO")) {
            std::vector<std::byte> buf(static_cast<std::size_t>(e.data_size));
            pkg.decrypt_range(e, 0, {buf.data(), buf.size()});
            return sfo_value(buf, want);
        }
    }
    return {};
}

void sfo_dump_all(const std::vector<std::byte>& d) {
    auto b = [&](std::size_t o) { return o < d.size() ? std::to_integer<unsigned>(d[o]) : 0u; };
    auto u16 = [&](std::size_t o) { return b(o) | (b(o + 1) << 8); };
    auto u32 = [&](std::size_t o) { return u16(o) | (u16(o + 2) << 16); };
    if (d.size() < 0x14 || b(0) != 0x00 || b(1) != 0x50 || b(2) != 0x53 || b(3) != 0x46) return;
    const unsigned key_start = u32(0x08), data_start = u32(0x0C), n = u32(0x10);
    for (unsigned i = 0; i < n; ++i) {
        const std::size_t base = 0x14 + static_cast<std::size_t>(i) * 16;
        if (base + 16 > d.size()) break;
        const unsigned key_off = u16(base), fmt = u16(base + 2), len = u32(base + 4), data_off = u32(base + 0x0C);
        std::string key;
        for (std::size_t k = key_start + key_off; k < d.size() && b(k); ++k) key.push_back(static_cast<char>(b(k)));
        const std::size_t dp = data_start + data_off;
        std::string s;
        if (fmt == 0x0404) {
            char hb[32];
            std::snprintf(hb, sizeof hb, "%u (0x%X)", u32(dp), u32(dp));
            s = hb;
        } else {
            for (std::size_t k = 0; k < len && dp + k < d.size() && b(dp + k); ++k) s.push_back(static_cast<char>(b(dp + k)));
        }
        std::printf("     %-16s = %s\n", key.c_str(), s.c_str());
    }
}

void dump_metadata(pkg::ps3_pkg_reader& pkg) {
    std::printf("   drm_type   : 0x%02X   content_type : 0x%02X\n", pkg.drm_type(), pkg.content_type());
}

void dry_run(const std::string& path) {
    std::printf("== %s\n", std::filesystem::path(path).filename().string().c_str());
    try {
        auto pkg = pkg::ps3_pkg_reader::from_file(path);
        std::uint64_t total = 0;
        int files = 0, dirs = 0;
        for (const auto& e : pkg.entries()) {
            if (e.is_directory) ++dirs;
            else { ++files; total += e.data_size; }
        }
        std::printf("   content id : %s\n", pkg.content_id().c_str());
        std::printf("   title id   : %s\n", pkg.title_id().c_str());
        dump_metadata(pkg);
        std::printf("   TITLE      : %s\n", pkg_sfo_value(pkg, "TITLE").c_str());
        std::printf("   CATEGORY   : %s\n", pkg_sfo_value(pkg, "CATEGORY").c_str());
        std::printf("   APP_VER    : %s   VERSION: %s\n", pkg_sfo_value(pkg, "APP_VER").c_str(), pkg_sfo_value(pkg, "VERSION").c_str());
        std::printf("   crypto     : %s\n", pkg.mode() == pkg::crypto_mode::aes_ctr ? "AES-128-CTR (retail)" : "SHA1-XOR (debug)");
        std::printf("   entries    : %d files, %d dirs, %s total\n", files, dirs, human(total).c_str());
        std::printf("   install to : game/%s/\n", pkg.title_id().c_str());
        for (const auto& e : pkg.entries()) {
            if (e.is_directory) continue;
            if (e.name == "PARAM.SFO" || (e.name.size() > 10 && e.name.substr(e.name.size() - 10) == "/PARAM.SFO")) {
                std::vector<std::byte> buf(static_cast<std::size_t>(e.data_size));
                pkg.decrypt_range(e, 0, {buf.data(), buf.size()});
                std::printf("   PARAM.SFO fields:\n");
                sfo_dump_all(buf);
                break;
            }
        }
        std::printf("   files:\n");
        int shown = 0;
        for (const auto& e : pkg.entries()) {
            if (e.is_directory) continue;
            if (shown++ >= 14) { std::printf("     ... and %d more\n", files - 14); break; }
            std::printf("     %-40s %-10s  flags=%08x %s\n", e.name.c_str(), human(e.data_size).c_str(), e.type_flags, e.overwrite ? "[OVERWRITE]" : "[keep]");
        }
    } catch (const std::exception& ex) {
        std::printf("   ERROR: %s\n", ex.what());
    }
    std::printf("\n");
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::printf("usage: ps3hdd_pkg_info <file.pkg | directory>\n");
        return 1;
    }
    std::printf("== pkg install dry run (read only) ==\n\n");

    namespace fs = std::filesystem;
    const std::string arg = argv[1];
    std::vector<std::string> pkgs;
    if (fs::is_directory(arg)) {
        for (auto& e : fs::recursive_directory_iterator(arg))
            if (e.is_regular_file() && e.path().extension() == ".pkg")
                pkgs.push_back(e.path().string());
        std::sort(pkgs.begin(), pkgs.end());
    } else {
        pkgs.push_back(arg);
    }

    std::printf("Found %zu package(s).\n\n", pkgs.size());
    for (const auto& p : pkgs) dry_run(p);
    return 0;
}