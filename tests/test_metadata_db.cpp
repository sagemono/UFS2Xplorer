#include <catch2/catch_test_macros.hpp>

#include <ps3hdd_mms/metadata_db.h>
#include <ps3hdd_crypto/be_io.h>

#include <array>
#include <cstddef>
#include <span>
#include <string>
#include <vector>

using namespace ps3hdd::mms;

static std::span<const std::byte> bytes(const char* s, std::size_t n) {
    return {reinterpret_cast<const std::byte*>(s), n};
}

TEST_CASE("CRC32 check value") {
    // ps3 uses poly 0x04C11DB7 with init 0
    REQUIRE(crc32_mpeg2(bytes("123456789", 9)) == 0x89A1897Fu);
}

TEST_CASE("CRC16CCITT check value and engine seed") {
    // seed 0xFFFF check: crc("123456789") == 0x29B1.
    REQUIRE(crc16_ccitt(bytes("123456789", 9), 0xFFFF) == 0x29B1);
    // default seed is 0x10FA
    REQUIRE(crc16_ccitt(bytes("NPEA00241", 9)) == crc16_ccitt(bytes("NPEA00241", 9)));
}

TEST_CASE("game_record round-trips and patches a string slot") {
    const std::uint16_t heap_start = 0x125 + 2 * 4;
    std::vector<std::byte> img(0x400, std::byte{0});
    auto put16 = [&](std::size_t o, std::uint16_t v) {
        img[o] = std::byte(v >> 8); img[o + 1] = std::byte(v & 0xff);
    };
    // slot0 -> heap string "NPEA00241"; slot1 empty (len 0)
    const std::string s = "NPEA00241";
    std::vector<std::byte> entry;
    for (char c : s) entry.push_back(std::byte(static_cast<std::uint8_t>(c)));
    entry.insert(entry.end(), 4, std::byte{0});
    entry.push_back(std::byte{0x0e});                 // typecode
    entry.insert(entry.end(), 4, std::byte{0});       // ptr
    put16(0x125 + 0, heap_start);                     // slot0 off
    put16(0x125 + 2, static_cast<std::uint16_t>(entry.size()));  // slot0 len
    put16(0x125 + 4, heap_start);                     // slot1 off (points at heap start)
    put16(0x125 + 6, 0);                              // slot1 len 0 (empty)
    for (std::size_t i = 0; i < entry.size(); ++i) img[heap_start + i] = entry[i];
    const std::size_t rec_end = heap_start + entry.size();

    auto rec = game_record::parse(img, 0, rec_end);
    REQUIRE(rec.get_string(0) == "NPEA00241");
    auto ser = rec.serialize();
    REQUIRE(std::vector<std::byte>(img.begin(), img.begin() + rec_end) == std::vector<std::byte>(ser.begin(), ser.begin() + rec_end));
    rec.set_string(0, "NPEB12345");
    auto ser2 = rec.serialize();
    auto rec2 = game_record::parse(ser2, 0, ser2.size());
    REQUIRE(rec2.get_string(0) == "NPEB12345");
}

TEST_CASE("btree_leaf insert/remove reproduces console leaf CRCs (1:1)") {
    using ps3hdd::read_be_u32;
    using ps3hdd::write_be_u32;
    std::vector<std::byte> node(0x1000, std::byte{0});
    write_be_u32(node.data() + 0x800, 0x03000000u);
    write_be_u32(node.data() + 0x808, 0x00035800u);
    write_be_u32(node.data() + 0x80c, 0x00000001u);
    auto lf = btree_leaf::parse(node);
    REQUIRE(lf.insert("NPEA00241"));
    REQUIRE(lf.insert("NPEA00333"));
    auto s2 = lf.serialize();
    REQUIRE(read_be_u32(s2.data() + 0x804) == 2u);
    REQUIRE(read_be_u32(s2.data() + 0x81c) == 0x9330ac10u);
    REQUIRE(lf.insert("NPEA00252"));
    auto s3 = lf.serialize();
    REQUIRE(read_be_u32(s3.data() + 0x81c) == 0x5689615cu);
    REQUIRE(lf.keys() == std::vector<std::string>{"NPEA00241","NPEA00252","NPEA00333"});
    std::size_t vp = 0x820 + 3*13;
    REQUIRE(read_be_u32(s3.data() + vp + 0)  == 0x04u);
    REQUIRE(read_be_u32(s3.data() + vp + 4)  == 0x04u);
    REQUIRE(read_be_u32(s3.data() + vp + 8)  == 0x10u);
    REQUIRE(read_be_u32(s3.data() + vp + 12) == 0x04u);
    REQUIRE(read_be_u32(s3.data() + vp + 16) == 0x1cu);
    REQUIRE(read_be_u32(s3.data() + vp + 20) == 0x04u);
    REQUIRE(lf.remove("NPEA00241"));
    REQUIRE(lf.remove("NPEA00252"));
    auto s1 = lf.serialize();
    REQUIRE(read_be_u32(s1.data() + 0x804) == 1u);
    REQUIRE(read_be_u32(s1.data() + 0x81c) == 0xd93be673u);
}
