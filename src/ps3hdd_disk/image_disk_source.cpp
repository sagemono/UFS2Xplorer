#include "image_disk_source.h"

#include <filesystem>
#include <stdexcept>

namespace ps3hdd::disk {

namespace {
void seek64(std::FILE* f, std::uint64_t offset) {
#if defined(_WIN32)
    _fseeki64(f, static_cast<long long>(offset), SEEK_SET);
#else
    std::fseek(f, static_cast<long>(offset), SEEK_SET);
#endif
}
} // namespace

image_disk_source::image_disk_source(const std::string& path, bool writable) : writable_(writable) {
    namespace fs = std::filesystem;
    if (!fs::exists(path))
        throw std::runtime_error("Disk image not found: " + path);

    file_ = std::fopen(path.c_str(), writable ? "r+b" : "rb");
    if (!file_)
        throw std::runtime_error("Failed to open disk image: " + path);

    total_size_ = static_cast<std::uint64_t>(fs::file_size(path));
    description_ = "Image: " + fs::path(path).filename().string() + " (" + format_size(total_size_) + ")";
}

image_disk_source::~image_disk_source() {
    if (file_) std::fclose(file_);
}

std::vector<std::byte> image_disk_source::read_bytes(std::uint64_t offset, std::size_t count) {
    std::lock_guard lock(mutex_);
    std::vector<std::byte> buf(count, std::byte{0});
    seek64(file_, offset);
    const std::size_t got = std::fread(buf.data(), 1, count, file_);
    (void)got;
    return buf;
}

std::vector<std::byte> image_disk_source::read_sectors(std::uint64_t start_sector, std::uint64_t count) {
    return read_bytes(start_sector * sector_size(), static_cast<std::size_t>(count * sector_size()));
}

void image_disk_source::write_bytes(std::uint64_t offset, std::span<const std::byte> data) {
    if (!writable_) throw std::runtime_error("Disk image opened read-only.");
    std::lock_guard lock(mutex_);
    seek64(file_, offset);
    const std::size_t wrote = std::fwrite(data.data(), 1, data.size(), file_);
    if (wrote != data.size()) throw std::runtime_error("Short write to disk image.");
}

void image_disk_source::write_sectors(std::uint64_t start_sector, std::span<const std::byte> data) {
    write_bytes(start_sector * sector_size(), data);
}

} // namespace ps3hdd::disk