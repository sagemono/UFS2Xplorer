#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>

namespace ps3hdd::disk {

struct sector_info {
    std::uint32_t logical = 512;
    std::uint32_t physical = 512;
};

class device_io_error : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

class raw_device {
public:
    virtual ~raw_device() = default;

    virtual std::uint64_t size() const = 0;
    virtual sector_info sectors() const = 0;
    virtual bool writable() const = 0;
    virtual std::uint32_t required_alignment() const = 0;
    virtual std::size_t read_at(std::uint64_t offset, std::span<std::byte> buf) = 0;
    virtual void write_at(std::uint64_t offset, std::span<const std::byte> buf) = 0;
    virtual std::string describe() const = 0;
};

std::unique_ptr<raw_device> open_raw_device(const std::string& path, bool writable);

std::string query_serial_number(const std::string& path);
bool eject_device(const std::string& path, bool& removed, bool& spundown, std::string& message);

} // namespace ps3hdd::disk