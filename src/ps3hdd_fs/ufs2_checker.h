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
    std::int64_t cs_array_mismatches = 0;
    std::int64_t orphan_inodes = 0;
    std::int64_t orphan_inodes_unlinked = 0;
    std::int64_t orphan_inodes_linked = 0;
    std::vector<std::int64_t> orphan_unlinked_inodes;
    std::vector<std::int64_t> orphan_all_inodes;
    std::int64_t cluster_map_mismatches = 0;
    std::int64_t cluster_sum_mismatches = 0;
    std::int64_t cstotal_mismatches = 0;
    std::vector<std::int64_t> used_but_free_frags;
    std::vector<std::string> findings; //human

    bool structurally_damaged() const { return cross_links != 0 || out_of_range != 0; }

    bool repairable() const {
        return used_but_free != 0 || summary_mismatches != 0 || cs_array_mismatches != 0 || cstotal_mismatches != 0 || cluster_map_mismatches != 0 || cluster_sum_mismatches != 0;
    }

    bool safe_to_write() const { return !structurally_damaged() && !repairable(); }

    bool clean() const { return safe_to_write() && orphan_inodes == 0; }

    std::string summary_line() const;
};

consistency_report check_consistency(ufs2_filesystem& fs, disk::disk_source& disk, std::size_t max_findings = 40);
std::vector<bool> claimed_fragment_map(ufs2_filesystem& fs, disk::disk_source& disk);

} // namespace ps3hdd::fs