#include "../common.h"

#include <ps3hdd_mms/metadata_db.h>

#include <cstddef>
#include <cstdio>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

using ps3hdd::tools::read_file;

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <mms/db folder | metadata_db_hdd file>\n", argv[0]);
        return 2;
    }
    std::filesystem::path arg(argv[1]);
    std::filesystem::path hdd = arg, idx;
    if (std::filesystem::is_directory(arg)) hdd = arg / "metadata_db_hdd";
    idx = hdd;
    idx += ".idx";

    const auto container = read_file(hdd);
    const auto index = read_file(idx);
    if (container.empty()) { std::fprintf(stderr, "cannot read %s\n", hdd.string().c_str()); return 1; }

    const auto rep = ps3hdd::mms::analyze(container, index);
    std::printf("container : %s  (%zu bytes, magic %s)\n", hdd.string().c_str(), container.size(), rep.container_magic_ok ? "OK" : "BAD");
    if (!index.empty())
        std::printf("index     : %s  (%zu bytes, CRCs %s, %d indexes / %d keys)\n", idx.string().c_str(), index.size(), rep.idx_crcs_ok ? "OK" : ("BAD @ " + std::to_string(rep.idx_bad_offset)).c_str(), rep.index_trees, rep.indexed_keys);
    if (rep.master_index_offset)
        std::printf("master idx: MmsBTree (type:id) embedded at 0x%zx\n", rep.master_index_offset);
    std::printf("games     : %zu\n", rep.games.size());
    for (const auto& g : rep.games) {
        std::printf("  @0x%06zx  %-10s cat=%-3s ver=%-6s sys=%-8s  %s\n", g.offset, g.title_id.c_str(), g.category.c_str(), g.version.c_str(), g.ps3_system_ver.c_str(), g.title.c_str());
    }
    return rep.container_magic_ok ? 0 : 1;
}