#include "physical_disk_source.h"

#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace ps3hdd::disk {

physical_disk_source::physical_disk_source(std::unique_ptr<raw_device> device, std::uint64_t total_size)
    : device_(std::move(device)) {
    if (!device_) throw std::invalid_argument("physical_disk_source: null device");
    total_size_ = total_size > 0 ? total_size : device_->size();
    alignment_ = std::max<std::uint32_t>(device_->required_alignment(), 1);
    description_ = "Physical: " + device_->describe() + " (" + format_size(total_size_) + ")";
}

std::vector<std::byte> physical_disk_source::read_sectors(std::uint64_t start_sector, std::uint64_t count) {
    const std::uint64_t offset = start_sector * sector_size();
    const std::size_t total = static_cast<std::size_t>(count * sector_size());
    std::vector<std::byte> buffer(total, std::byte{0});

    std::lock_guard lock(mutex_);
    std::size_t done = 0;
    std::uint64_t remaining = count;
    while (remaining > 0) {
        std::uint32_t chunk = static_cast<std::uint32_t>(std::min<std::uint64_t>(remaining, max_sectors_per_read_));
        const std::size_t chunk_bytes = static_cast<std::size_t>(chunk) * sector_size();
        try {
            std::size_t got = device_->read_at(offset + done, {buffer.data() + done, chunk_bytes});
            if (got == 0) break; // EOF
            done += got;
            remaining -= got / sector_size();
        } catch (const device_io_error&) {
            if (chunk <= min_sectors_per_read) throw;
            max_sectors_per_read_ = std::max(min_sectors_per_read, chunk / 2);
        }
    }
    return buffer;
}

std::vector<std::byte> physical_disk_source::read_bytes(std::uint64_t offset, std::size_t count) {
    const std::uint32_t ss = sector_size();
    const std::uint64_t aligned_start = (offset / ss) * ss;
    const std::uint64_t aligned_end = ((offset + count + ss - 1) / ss) * ss;
    const std::uint64_t start_sector = aligned_start / ss;
    const std::uint64_t sectors = (aligned_end - aligned_start) / ss;

    std::vector<std::byte> aligned = read_sectors(start_sector, sectors);
    const std::size_t delta = static_cast<std::size_t>(offset - aligned_start);
    std::vector<std::byte> result(count, std::byte{0});
    const std::size_t avail = aligned.size() > delta ? aligned.size() - delta : 0;
    std::memcpy(result.data(), aligned.data() + delta, std::min(count, avail));
    return result;
}

void physical_disk_source::write_bytes(std::uint64_t offset, std::span<const std::byte> data) {
    if (!device_->writable()) throw std::runtime_error("Disk opened read-only.");
    if (offset % sector_size() != 0 || data.size() % sector_size() != 0)
        throw std::invalid_argument("Physical disk writes must be sector-aligned.");

    std::lock_guard lock(mutex_);

    const bool ptr_aligned = (reinterpret_cast<std::uintptr_t>(data.data()) % alignment_) == 0;
    const bool len_aligned = (data.size() % alignment_) == 0 && (offset % alignment_) == 0;

    if (ptr_aligned && len_aligned) {
        device_->write_at(offset, data);
        return;
    }

    write_scratch_.reset(data.size(), alignment_);
    std::memcpy(write_scratch_.data(), data.data(), data.size());
    device_->write_at(offset, {write_scratch_.data(), data.size()});
}


void physical_disk_source::write_sectors(std::uint64_t start_sector, std::span<const std::byte> data) {
    write_bytes(start_sector * sector_size(), data);
}

} // namespace ps3hdd::disk