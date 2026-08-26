#pragma once

#include "ufs2_filesystem.h"

#include <ps3hdd_disk/disk_source.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace ps3hdd::fs {

struct consistency_report {
    std::int64_t inodes_walked = 0;
    std::int64_t fragments_claimed = 0;
    std::int64_t cross_links = 0;
    std::int64_t out_of_range = 0;
    std::int64_t used_but_free = 0;
    std::int64_t summary_mismatches = 0;
    std::vector<std::string> findings; //human

    bool clean() const {
        return cross_links == 0 && out_of_range == 0 && used_but_free == 0 && summary_mismatches == 0;
    }
};

consistency_report check_consistency(ufs2_filesystem& fs, disk::disk_source& disk, std::size_t max_findings = 40);

} // namespace ps3hdd::fs
