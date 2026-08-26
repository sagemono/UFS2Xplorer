#pragma once

#include <ps3hdd_disk/disk_source.h>

#include <cstring>
#include <vector>

namespace th {

class memory_disk_source : public ps3hdd::disk::disk_source {
public:
    explicit memory_disk_source(std::size_t size) : store_(size, std::byte{0}) {}

    std::vector<std::byte>& store() { return store_; }

    std::uint64_t total_size() const override { return store_.size(); }
    std::uint32_t sector_size() const override { return 512; }
    std::string description() const override { return "memory"; }
    bool can_write() const override { return true; }

    std::vector<std::byte> read_bytes(std::uint64_t offset, std::size_t count) override {
        std::vector<std::byte> out(count, std::byte{0});
        if (offset < store_.size()) {
            const std::size_t n = std::min<std::size_t>(count, store_.size() - offset);
            std::memcpy(out.data(), store_.data() + offset, n);
        }
        return out;
    }
    std::vector<std::byte> read_sectors(std::uint64_t start_sector, std::uint64_t count) override {
        return read_bytes(start_sector * 512, static_cast<std::size_t>(count * 512));
    }
    void write_bytes(std::uint64_t offset, std::span<const std::byte> data) override {
        if (offset + data.size() > store_.size()) store_.resize(offset + data.size(), std::byte{0});
        std::memcpy(store_.data() + offset, data.data(), data.size());
    }
    void write_sectors(std::uint64_t start_sector, std::span<const std::byte> data) override {
        write_bytes(start_sector * 512, data);
    }

private:
    std::vector<std::byte> store_;
};

} // namespace th