#include "test_helpers.h"

#include <ps3hdd_disk/physical_disk_source.h>
#include <ps3hdd_disk/raw_device.h>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <cstring>

using namespace ps3hdd::disk;

namespace {

// in memory raw_device for exercising physical_disk_source's portable logic.
class mem_device : public raw_device {
public:
    std::vector<std::byte> store;
    std::uint32_t align = 512;
    std::size_t max_ok_read = SIZE_MAX; // reads larger than this throw
    bool writ = true;
    std::uintptr_t last_write_ptr = 0; // to check bounce-buffer alignment
    int oversize_read_failures = 0;

    std::uint64_t size() const override { return store.size(); }
    sector_info sectors() const override { return {align, align}; }
    bool writable() const override { return writ; }
    std::uint32_t required_alignment() const override { return align; }
    std::string describe() const override { return "mem"; }

    std::size_t read_at(std::uint64_t offset, std::span<std::byte> buf) override {
        if (buf.size() > max_ok_read) {
            ++oversize_read_failures;
            throw device_io_error("simulated oversize read rejection");
        }
        if (offset >= store.size()) return 0;
        const std::size_t n = std::min<std::size_t>(buf.size(), store.size() - offset);
        std::memcpy(buf.data(), store.data() + offset, n);
        return n;
    }

    void write_at(std::uint64_t offset, std::span<const std::byte> buf) override {
        last_write_ptr = reinterpret_cast<std::uintptr_t>(buf.data());
        if (offset + buf.size() > store.size()) store.resize(offset + buf.size());
        std::memcpy(store.data() + offset, buf.data(), buf.size());
    }
};

std::unique_ptr<mem_device> dev_with(std::size_t bytes, std::uint64_t seed) {
    auto d = std::make_unique<mem_device>();
    d->store = th::rng(bytes, seed);
    return d;
}

} // namespace

TEST_CASE("physical read reassembles multi-chunk transfers", "[disk][phys]") {
    auto d = dev_with(1u << 20, 5); // 1 MB
    const auto expected = d->store;
    physical_disk_source src(std::move(d));

    auto all = src.read_sectors(0, (1u << 20) / 512);
    REQUIRE(all == expected);
}

TEST_CASE("physical read halves the transfer size on device rejection", "[disk][phys]") {
    auto d = dev_with(1u << 20, 6);
    const auto expected = d->store;
    d->max_ok_read = 100 * 1024; // reject anything over ~100 KB
    auto* raw = d.get();
    physical_disk_source src(std::move(d));

    auto all = src.read_sectors(0, (1u << 20) / 512);
    REQUIRE(all == expected); // data still correct
    REQUIRE(raw->oversize_read_failures > 0); // fallback actually triggered
    REQUIRE(src.max_sectors_per_read() * 512u <= 100u * 1024u); // settled below the cap
    REQUIRE(src.max_sectors_per_read() >= 128u); // but not below the floor
}

TEST_CASE("physical read_bytes aligns and slices", "[disk][phys]") {
    auto d = dev_with(8 * 512, 7);
    const auto expected = d->store;
    physical_disk_source src(std::move(d));

    auto slice = src.read_bytes(600, 1000);
    REQUIRE(slice.size() == 1000);
    REQUIRE(std::equal(slice.begin(), slice.end(), expected.begin() + 600));
}

TEST_CASE("physical writes bounce through an aligned buffer when needed", "[disk][phys]") {
    auto d = std::make_unique<mem_device>();
    d->store.assign(4 * 512, std::byte{0});
    d->align = 4096; // force the aligned buffer path for a 512 byte write
    auto* raw = d.get();
    physical_disk_source src(std::move(d));

    // the 512 byte write is sector aligned but not 4096 aligned, so it must bounce
    const auto payload = th::rng(512, 8);
    src.write_bytes(512, th::cspan(payload));

    REQUIRE(raw->last_write_ptr % 4096u == 0); // device saw aligned memory
    REQUIRE(std::equal(payload.begin(), payload.end(), raw->store.begin() + 512)); // data correct
}

TEST_CASE("physical rejects unaligned writes", "[disk][phys]") {
    auto d = dev_with(4 * 512, 9);
    physical_disk_source src(std::move(d));
    const auto payload = th::rng(500, 10); // not a mult of 512!
    REQUIRE_THROWS(src.write_bytes(512, th::cspan(payload)));
    REQUIRE_THROWS(src.write_bytes(100, th::cspan(th::rng(512, 11)))); // unaligned offset
}