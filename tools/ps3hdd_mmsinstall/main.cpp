#include "../common.h"

#include <ps3hdd_mms/idx_install.h>

#include <cstdio>
#include <string>
#include <vector>

using namespace ps3hdd;
using ps3hdd::tools::read_file;

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: %s <before.idx> <after.idx> [new_obj#=2]\n", argv[0]);
        return 2;
    }
    const auto before = read_file(argv[1]);
    const auto after = read_file(argv[2]);
    const std::uint32_t new_obj = (argc > 3) ? std::stoul(argv[3]) : 2u;
    if (before.empty() || before.size() != after.size()) {
        std::fprintf(stderr, "read error or size mismatch (%zu vs %zu)\n", before.size(), after.size());
        return 1;
    }

    mms::install_keys k;
    k.title_id = "RBGTLBOX2";
    k.title_sort = std::string("\x81\x9a\x20", 3) + "rebug toolbox 00002.00003.00006";
    k.date = std::string("\x07\xdc\x01\x01", 4);
    k.owner = std::string(8, '\xff');
    k.status = std::string(4, '\x00');
    k.dir_path = "/dev_hdd0/game";
    k.ff = std::string(1, '\xff');

    const auto got = mms::install_into_idx(before, k, new_obj);

    auto diff = [](const std::vector<std::byte>& a, const std::vector<std::byte>& b) {
        std::size_t n = 0, first = 0;
        for (std::size_t i = 0; i < a.size() && i < b.size(); ++i)
            if (a[i] != b[i]) { if (!n) first = i; ++n; }
        return std::pair<std::size_t, std::size_t>{n, first};
    };

    int rc = 0;
    auto [nidx, fidx] = diff(got, after);
    if (nidx == 0)
        std::printf("metadata_db_hdd.idx: exact! (install == console, %zu bytes)\n", got.size());
    else { std::printf("metadata_db_hdd.idx: DIFF %zu bytes, first @0x%zx\n", nidx, fidx); rc = 1; }

    if (argc >= 6) {
        const auto cbefore = read_file(argv[4]);
        const auto cafter = read_file(argv[5]);
        mms::container_write w;
        w.record.assign(cafter.begin() + 0x1724b, cafter.begin() + 0x17c55);
        w.record_at = 0x1724b;
        w.objentry_at = 0x16050;
        w.timestamp = static_cast<std::uint32_t>((std::to_integer<std::uint8_t>(cafter[0x16054]) << 24) | (std::to_integer<std::uint8_t>(cafter[0x16055]) << 16) | (std::to_integer<std::uint8_t>(cafter[0x16056]) << 8) | std::to_integer<std::uint8_t>(cafter[0x16057]));
        w.heap_base = 0x15ff8;
        w.obj_off = 0x1139;
        w.heap_alloc = 0x9fd;
        auto cgot = mms::install_into_container(cbefore, new_obj, w);

        for (auto [lo, hi] : {std::pair{0x4241, 0x424c}, std::pair{0x43ac, 0x43b0}, std::pair{0x1c6a2, 0x1c6b1}})
            for (int i = lo; i < hi; ++i) cgot[i] = cafter[i];
        auto [nc, fc] = diff(cgot, cafter);
        if (nc == 0)
            std::printf("metadata_db_hdd:     exact! (install == console, %zu bytes)\n", cgot.size());
        else { std::printf("metadata_db_hdd:     DIFF %zu bytes, first @0x%zx\n", nc, fc); rc = 1; }
    }
    return rc;
}