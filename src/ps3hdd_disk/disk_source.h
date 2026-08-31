#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace ps3hdd::disk {

class disk_source {
public:
    virtual ~disk_source() = default;

    virtual std::uint64_t total_size() const = 0;
    virtual std::uint32_t sector_size() const = 0;
    std::uint64_t sector_count() const { return total_size() / sector_size(); }
    virtual std::string description() const = 0;
    virtual bool can_write() const = 0;

    virtual std::vector<std::byte> read_sectors(std::uint64_t start_sector, std::uint64_t count) = 0;
    virtual std::vector<std::byte> read_bytes(std::uint64_t offset, std::size_t count) = 0;

    virtual void write_sectors(std::uint64_t start_sector, std::span<const std::byte> data) = 0;
    virtual void write_bytes(std::uint64_t offset, std::span<const std::byte> data) = 0;
    virtual void flush() {}
};

std::string format_size(std::uint64_t bytes);

} // namespace ps3hdd::disk