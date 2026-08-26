#pragma once

#include "disk_source.h"

#include <cstdio>
#include <mutex>
#include <string>

namespace ps3hdd::disk {

class image_disk_source : public disk_source {
public:
    explicit image_disk_source(const std::string& path, bool writable = false);
    ~image_disk_source() override;

    std::uint64_t total_size() const override { return total_size_; }
    std::uint32_t sector_size() const override { return 512; }
    std::string description() const override { return description_; }
    bool can_write() const override { return writable_; }

    std::vector<std::byte> read_sectors(std::uint64_t start_sector, std::uint64_t count) override;
    std::vector<std::byte> read_bytes(std::uint64_t offset, std::size_t count) override;
    void write_sectors(std::uint64_t start_sector, std::span<const std::byte> data) override;
    void write_bytes(std::uint64_t offset, std::span<const std::byte> data) override;

private:
    std::FILE* file_ = nullptr;
    bool writable_ = false;
    std::uint64_t total_size_ = 0;
    std::string description_;
    std::mutex mutex_;
};

} // namespace ps3hdd::disk