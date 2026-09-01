#pragma once

#include <ps3hdd_disk/disk_source.h>

#include <algorithm>
#include <cstring>
#include <map>
#include <vector>

namespace th {

class sparse_disk_source : public ps3hdd::disk::disk_source {
public:
    static constexpr std::size_t kChunk = 64 * 1024;

    explicit sparse_disk_source(std::uint64_t size) : size_(size) {}

    std::uint64_t total_size() const override { return size_; }
    std::uint32_t sector_size() const override { return 512; }
    std::string description() const override { return "sparse"; }
    bool can_write() const override { return true; }

    // avoid OOM... 
    std::size_t resident_bytes() const { return chunks_.size() * kChunk; }
    std::size_t resident_chunks() const { return chunks_.size(); }

    std::vector<std::byte> read_bytes(std::uint64_t offset, std::size_t count) override {
        std::vector<std::byte> out(count, std::byte{0});
        std::size_t done = 0;
        while (done < count) {
            const std::uint64_t pos = offset + done;
            const std::uint64_t idx = pos / kChunk;
            const std::size_t within = static_cast<std::size_t>(pos % kChunk);
            const std::size_t n = std::min(count - done, kChunk - within);
            if (auto it = chunks_.find(idx); it != chunks_.end())
                std::memcpy(out.data() + done, it->second.data() + within, n);
            done += n;
        }
        return out;
    }

    std::vector<std::byte> read_sectors(std::uint64_t start_sector, std::uint64_t count) override {
        return read_bytes(start_sector * 512, static_cast<std::size_t>(count * 512));
    }

    void write_bytes(std::uint64_t offset, std::span<const std::byte> data) override {
        std::size_t done = 0;
        while (done < data.size()) {
            const std::uint64_t pos = offset + done;
            const std::uint64_t idx = pos / kChunk;
            const std::size_t within = static_cast<std::size_t>(pos % kChunk);
            const std::size_t n = std::min(data.size() - done, kChunk - within);
            auto it = chunks_.find(idx);
            if (it == chunks_.end())
                it = chunks_.emplace(idx, std::vector<std::byte>(kChunk, std::byte{0})).first;
            std::memcpy(it->second.data() + within, data.data() + done, n);
            done += n;
        }
    }

    void write_sectors(std::uint64_t start_sector, std::span<const std::byte> data) override {
        write_bytes(start_sector * 512, data);
    }

private:
    std::uint64_t size_;
    std::map<std::uint64_t, std::vector<std::byte>> chunks_;
};

} // namespace th