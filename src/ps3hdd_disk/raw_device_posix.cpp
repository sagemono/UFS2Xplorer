#include "raw_device.h"

#if defined(__linux__) || defined(__APPLE__)

#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <string>

#if defined(__linux__)
#  include <linux/fs.h>   // BLKGETSIZE64, BLKSSZGET, BLKPBSZGET
#elif defined(__APPLE__)
#  include <sys/disk.h>   // DKIOCGETBLOCKSIZE, DKIOCGETBLOCKCOUNT, DKIOCGETPHYSICALBLOCKSIZE
#endif

namespace ps3hdd::disk {

namespace {

sector_info query_sizes_fd(int fd) {
    sector_info info{};
#if defined(__linux__)
    int logical = 0, physical = 0;
    if (ioctl(fd, BLKSSZGET, &logical) == 0 && logical > 0) info.logical = static_cast<std::uint32_t>(logical);
    if (ioctl(fd, BLKPBSZGET, &physical) == 0 && physical > 0) info.physical = static_cast<std::uint32_t>(physical);
    else info.physical = info.logical;
#elif defined(__APPLE__)
    std::uint32_t bs = 0, pbs = 0;
    if (ioctl(fd, DKIOCGETBLOCKSIZE, &bs) == 0 && bs > 0) info.logical = bs;
    if (ioctl(fd, DKIOCGETPHYSICALBLOCKSIZE, &pbs) == 0 && pbs > 0) info.physical = pbs;
    else info.physical = info.logical;
#endif
    return info;
}

std::uint64_t query_size_fd(int fd) {
#if defined(__linux__)
    std::uint64_t bytes = 0;
    if (ioctl(fd, BLKGETSIZE64, &bytes) == 0) return bytes;
#elif defined(__APPLE__)
    std::uint32_t block = 0;
    std::uint64_t count = 0;
    if (ioctl(fd, DKIOCGETBLOCKSIZE, &block) == 0 && ioctl(fd, DKIOCGETBLOCKCOUNT, &count) == 0)
        return count * block;
#endif
    off_t end = lseek(fd, 0, SEEK_END);
    return end > 0 ? static_cast<std::uint64_t>(end) : 0;
}

class posix_raw_device : public raw_device {
public:
    posix_raw_device(const std::string& path, bool writable) : path_(path), writable_(writable) {
        fd_ = ::open(path.c_str(), writable ? O_RDWR : O_RDONLY);
        if (fd_ < 0)
            throw device_io_error("open failed for " + path + ": " + std::strerror(errno));
        sectors_ = query_sizes_fd(fd_);
        size_ = query_size_fd(fd_);
    }

    ~posix_raw_device() override {
        if (fd_ >= 0) ::close(fd_);
    }

    std::uint64_t size() const override { return size_; }
    sector_info sectors() const override { return sectors_; }
    bool writable() const override { return writable_; }
    std::uint32_t required_alignment() const override { return sectors_.logical; }
    std::string describe() const override { return path_; }

    std::size_t read_at(std::uint64_t offset, std::span<std::byte> buf) override {
        std::size_t done = 0;
        while (done < buf.size()) {
            ssize_t r = ::pread(fd_, buf.data() + done, buf.size() - done, static_cast<off_t>(offset + done));
            if (r < 0) {
                if (errno == EINTR) continue;
                throw device_io_error("pread failed at offset " + std::to_string(offset) + ": " + std::strerror(errno));
            }
            if (r == 0) break; // EOF
            done += static_cast<std::size_t>(r);
        }
        return done;
    }

    void write_at(std::uint64_t offset, std::span<const std::byte> buf) override {
        std::size_t done = 0;
        while (done < buf.size()) {
            ssize_t w = ::pwrite(fd_, buf.data() + done, buf.size() - done, static_cast<off_t>(offset + done));
            if (w < 0) {
                if (errno == EINTR) continue;
                throw device_io_error("pwrite failed at offset " + std::to_string(offset) + ": " + std::strerror(errno));
            }
            done += static_cast<std::size_t>(w);
        }
    }

private:
    std::string path_;
    bool writable_;
    int fd_ = -1;
    std::uint64_t size_ = 0;
    sector_info sectors_{};
};

} // namespace

std::unique_ptr<raw_device> open_raw_device(const std::string& path, bool writable) {
    return std::make_unique<posix_raw_device>(path, writable);
}

std::string query_serial_number(const std::string& /*path*/) {
    return {};
}

bool eject_device(const std::string& /*path*/, bool& removed, bool& spundown, std::string& message) {
    removed = false;
    spundown = false;
    message = "Eject/spin-down is not implemented on this platform yet";
    return false;
}

} // namespace ps3hdd::disk

#endif // __linux__ || __APPLE__