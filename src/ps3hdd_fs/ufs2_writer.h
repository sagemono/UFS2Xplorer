#pragma once

#include "ffs_policy.h"
#include "ufs2_filesystem.h"
#include "ufs2_types.h"

#include <ps3hdd_disk/disk_source.h>

#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <set>
#include <vector>

namespace ps3hdd::fs {

struct cylinder_group {
    int number = 0;
    std::uint64_t disk_offset = 0;
    std::vector<std::byte> raw_data;

    std::uint32_t magic = 0;          // 0x04, expect 0x00090255
    std::int32_t num_data_blocks = 0; // 0x14 cg_ndblk
    std::int32_t free_blocks = 0;     // 0x1C cs_nbfree
    std::int32_t free_inodes = 0;     // 0x20 cs_nifree
    std::int32_t inodes_used_offset = 0; // 0x5C cg_iusedoff
    std::int32_t free_blocks_offset = 0; // 0x60 cg_freeoff
    std::int32_t block_rotor = 0;     // 0x28 cg_rotor
    std::int32_t frag_rotor = 0;      // 0x2C cg_frotor
    std::int32_t inode_rotor = 0;     // 0x30 cg_irotor
    std::int32_t inited_iblk = 0;     // 0x78 cg_initediblk

    static constexpr std::uint32_t magic_value = 0x00090255;
};

class ufs2_writer {
public:
    ufs2_writer(ufs2_filesystem& fs, disk::disk_source& disk);
    ~ufs2_writer();

    cylinder_group& read_cylinder_group(int cg_number);
    void write_cylinder_group(cylinder_group& cg);
    void flush_dirty_cgs();

    int find_free_inode(const cylinder_group& cg) const;

    std::pair<std::int64_t, int> find_free_block_run(cylinder_group& cg, int frags_per_block, int max_blocks);

    std::int64_t find_free_fragments(cylinder_group& cg, int count, int pref_local = -1);

    void mark_inode_used(cylinder_group& cg, int inode_idx);
    void mark_fragments_used(cylinder_group& cg, int start_frag, int count);
    void mark_fragment_used(cylinder_group& cg, int frag_idx);

    std::set<std::int64_t>& protected_fragments() { return protected_fragments_; }

    std::vector<std::byte> build_file_inode(std::int64_t file_size, std::span<const std::int64_t> direct_blocks, std::int64_t indirect = 0, std::int64_t double_indirect = 0, std::int64_t triple_indirect = 0);
    std::vector<std::byte> build_directory_inode(std::int64_t data_block_frag, int nlink, std::uint16_t mode = 0x41FF);
    std::vector<std::byte> build_empty_directory_block(std::uint64_t self_inode, std::uint64_t parent_inode);

    static std::vector<std::byte> add_entry_to_directory_block(std::span<const std::byte> dir_block, std::uint64_t inode, const std::string& name, std::uint8_t dir_entry_type);

    void write_inode(std::uint64_t inode_number, std::span<const std::byte> inode_data);
    void write_data_block(std::int64_t fragment_address, std::span<const std::byte> data);

    std::uint64_t create_directory(std::uint64_t parent_inode, const std::string& name);
    std::uint64_t write_file(std::uint64_t parent_inode, const std::string& name, std::span<const std::byte> data);

    std::uint64_t write_file(std::uint64_t parent_inode, const std::string& name, std::int64_t size, const std::function<void(std::span<std::byte>)>& fill, const std::function<void(std::int64_t)>& on_written = {});

    void delete_file(std::uint64_t parent_inode, const std::string& name);
    void delete_directory(std::uint64_t parent_inode, const std::string& name);
    void delete_tree(std::uint64_t parent_inode, const std::string& name);

    void move_entry(std::uint64_t src_parent, const std::string& old_name, std::uint64_t dest_parent, const std::string& new_name);

    void update_superblock();
    int repair_free_counts(const std::function<void(int done, int total)>& progress = {});
    int repair_used_but_free(const std::vector<std::int64_t>& frags);
    int reclaim_orphan_inodes(const std::vector<std::int64_t>& inodes, const std::vector<bool>& claimed);
    void set_clock(std::uint64_t unix_seconds) { clock_ = unix_seconds; }

    bool set_lv2_policy(bool on);
    bool lv2_policy() const { return use_lv2_policy_; }
    const ffs_context& policy() const { return policy_; }

private:
    ufs2_filesystem& fs_;
    disk::disk_source& disk_;
    const superblock& sb_;

    std::map<int, cylinder_group> cg_cache_;
    std::set<int> dirty_cgs_;
    std::set<std::int64_t> protected_fragments_;
    std::map<int, int> cg_search_cursor_;

    ffs_context policy_;
    bool use_lv2_policy_ = false;
    bool load_policy_context();
    void refresh_policy_summary(const cylinder_group& cg);

    std::map<int, std::array<std::int64_t, 5>> cg_summary_base_;
    std::array<std::int64_t, 5> compute_cg_summary(const cylinder_group& cg) const;

    int frags_per_block() const { return static_cast<int>(sb_.block_size / sb_.fragment_size); }

    std::uint64_t clock_ = 0; // 0 = real wall time
    std::uint32_t gen_ = 0x1234abcdu;
    std::uint64_t now() const;
    std::uint32_t next_gen() { gen_ = gen_ * 1664525u + 1013904223u; return gen_; }
    void patch_bytes(std::uint64_t offset, std::span<const std::byte> data);

    bool directory_contains_entry(std::uint64_t parent_inode, const std::string& name);
    void add_entry_to_directory(std::uint64_t parent_inode, std::uint64_t child_inode, const std::string& name, std::uint8_t dir_entry_type);
    std::uint64_t allocate_inode(int start_cg, cylinder_group*& out_cg, int& out_idx);
    std::int64_t alloc_run(int count, cylinder_group*& block_cg);
    std::int64_t alloc_run_pref(int count, std::int64_t pref, cylinder_group*& block_cg);
    bool fragments_free_at(const cylinder_group& cg, int start, int count) const;
    int rotor_start_for(const cylinder_group& cg, int count) const;
    void store_rotor(cylinder_group& cg, int count, int frag);
    int find_free_inode_rotor(cylinder_group& cg);
    std::int64_t find_free_fragments_scan(cylinder_group& cg, int count, int from, int to);
    bool frag_extend(std::int64_t abs_frag, int ofrags, int nfrags);

    std::vector<int> block_free_runs(const cylinder_group& cg, int block_start, int fpb) const;
    void update_frsum(cylinder_group& cg, const std::vector<int>& old_runs, const std::vector<int>& new_runs, int fpb);
    void cluster_acct(cylinder_group& cg, int blkno, int cnt);

    void mark_fragment_free(cylinder_group& cg, int frag_idx);
    void mark_fragments_free(cylinder_group& cg, int start_frag, int count);
    void mark_inode_free(cylinder_group& cg, int inode_idx);
    void free_block_run(std::int64_t abs_frag, int count);
    void free_inode_blocks(const inode& in);
    std::uint64_t remove_entry_from_directory(std::uint64_t parent_inode, const std::string& name);
    void truncate_directory_tail(std::uint64_t dir_inode);
};

} // namespace ps3hdd::fs