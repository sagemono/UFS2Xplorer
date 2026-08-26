#include "test_helpers.h"

#include <ps3hdd_disk/image_disk_source.h>

#include <catch2/catch_test_macros.hpp>

#include <cstdio>
#include <filesystem>
#include <string>

using namespace ps3hdd::disk;

namespace {
std::string make_temp_image(const std::vector<std::byte>& content) {
    namespace fs = std::filesystem;
    auto path = (fs::temp_directory_path() / fs::path("ps3hdd_img_" + std::to_string(std::rand()) + ".bin")).string();
    std::FILE* f = std::fopen(path.c_str(), "wb");
    std::fwrite(content.data(), 1, content.size(), f);
    std::fclose(f);
    return path;
}
}

TEST_CASE("image_disk_source reads sectors and byte ranges", "[disk][image]") {
    const auto data = th::rng(8 * 512, 100);
    const auto path = make_temp_image(data);

    {
        image_disk_source src(path);
        REQUIRE(src.total_size() == data.size());
        REQUIRE(src.sector_size() == 512);
        REQUIRE(src.sector_count() == 8);
        REQUIRE_FALSE(src.can_write());

        auto s2 = src.read_sectors(2, 3);
        REQUIRE(std::equal(s2.begin(), s2.end(), data.begin() + 2 * 512));

        auto bytes = src.read_bytes(600, 1000);
        REQUIRE(std::equal(bytes.begin(), bytes.end(), data.begin() + 600));
    }
    std::filesystem::remove(path);
}

TEST_CASE("image_disk_source round trips writes", "[disk][image]") {
    const auto data = th::rng(4 * 512, 101);
    const auto path = make_temp_image(data);

    {
        image_disk_source src(path, /*writable=*/true);
        REQUIRE(src.can_write());
        const auto patch = th::rng(512, 202);
        src.write_sectors(1, th::cspan(patch));
        auto back = src.read_sectors(1, 1);
        REQUIRE(std::equal(back.begin(), back.end(), patch.begin()));
    }

    std::filesystem::remove(path);
}

TEST_CASE("image_disk_source zero fills reads past EOF", "[disk][image]") {
    const auto data = th::rng(512, 103);
    const auto path = make_temp_image(data);

    {
        image_disk_source src(path);
        auto over = src.read_bytes(256, 512); // 256 real bytes, 256 past EOF
        REQUIRE(over.size() == 512);
        REQUIRE(std::equal(over.begin(), over.begin() + 256, data.begin() + 256));
        for (std::size_t i = 256; i < 512; ++i) REQUIRE(over[i] == std::byte{0});
    }
    std::filesystem::remove(path);
}