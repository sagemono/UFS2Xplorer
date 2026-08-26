#include "disk_source.h"

#include <array>
#include <cstdio>

namespace ps3hdd::disk {

std::string format_size(std::uint64_t bytes) {
    static constexpr std::array<const char*, 5> units{"B", "KB", "MB", "GB", "TB"};
    double size = static_cast<double>(bytes);
    std::size_t unit = 0;
    while (size >= 1024.0 && unit + 1 < units.size()) {
        size /= 1024.0;
        ++unit;
    }
    char buf[64];
    std::snprintf(buf, sizeof buf, "%.2f %s", size, units[unit]);
    return buf;
}

} // namespace ps3hdd::disk