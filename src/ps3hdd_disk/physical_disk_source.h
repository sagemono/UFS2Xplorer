#pragma once

#include "aligned_buffer.h"
#include "disk_source.h"
#include "raw_device.h"

#include <memory>
#include <mutex>

namespace ps3hdd::disk {

class physical_disk_source : public disk_source {
public:
    explicit physical_disk_source(std::unique_ptr<raw_device> device, std::uint64_t total_size = 0);

    std::uint64_t total_size() const override { return total_size_; }
    std::uint32_t sector_size() const override { return 512; }
    std::string description() const override { return description_; }
    bool can_write() const override { return device_->writable(); }

    std::vector<std::byte> read_sectors(std::uint64_t start_sector, std::uint64_t count) override;
    std::vector<std::byte> read_bytes(std::uint64_t offset, std::size_t count) override;
    void write_sectors(std::uint64_t start_sector, std::span<const std::byte> data) override;
    void write_bytes(std::uint64_t offset, std::span<const std::byte> data) override;

    std::uint32_t max_sectors_per_read() const { return max_sectors_per_read_; }

private:
    std::unique_ptr<raw_device> device_;
    std::uint64_t total_size_ = 0;
    std::string description_;
    std::mutex mutex_;

    std::uint32_t max_sectors_per_read_ = 8192;
    static constexpr std::uint32_t min_sectors_per_read = 128;

    aligned_buffer write_scratch_;
    std::uint32_t alignment_ = 4096;
};

} // namespace ps3hdd::disk