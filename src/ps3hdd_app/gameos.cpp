#include "gameos.h"

#include <ps3hdd_crypto/be_io.h>
#include <ps3hdd_cryptodisk/encrypted_disk_source.h>
#include <ps3hdd_cryptodisk/encrypted_disk_source_cbc.h>
#include <ps3hdd_fs/ufs2_filesystem.h>

#include <stdexcept>
#include <vector>

namespace ps3hdd::app {

namespace {

std::uint32_t be32(const std::vector<std::byte>& d, std::size_t o) { return ps3hdd::read_be_u32(d, o); }
std::uint64_t be64(const std::vector<std::byte>& d, std::size_t o) { return ps3hdd::read_be_u64(d, o); }
bool is_pow2(std::uint32_t v) { return v && (v & (v - 1)) == 0; }
bool sane_sb(const std::vector<std::byte>& sb) {
    const std::uint32_t bs = be32(sb, 0x30), fsz = be32(sb, 0x34), ncg = be32(sb, 0x2C);
    return is_pow2(bs) && bs >= 4096 && bs <= 65536 && is_pow2(fsz) && fsz >= 512 && fsz <= bs && ncg >= 1 && ncg < 2000000 && be32(sb, 0xB8) > 0;
}
bool table_magic(const std::vector<std::byte>& s0) {
    return be32(s0, 0x14) == 0x0FACE0FFu || be32(s0, 0x1C) == 0xDEADFACEu;
}

// try to find the best GameOS partition in a decrypted view
int best_partition(disk::disk_source& dec, std::uint64_t sector_count, std::uint64_t total_size, std::uint64_t& out_start) {
    std::vector<std::uint64_t> starts = {0x20, 0x80010, 0x400000, 0x800};
    auto header = dec.read_bytes(0, 2048);
    for (std::size_t i = 0; i < 8; ++i) {
        const std::uint64_t ps = be64(header, 0x30 + i * 0x90);
        if (ps > 0 && ps < sector_count) starts.push_back(ps);
    }
    int best_ncg = -1;
    for (auto ps : starts) {
        try {
            const std::uint64_t sb_off = ps * 512 + 65536;
            if (sb_off + 8192 > total_size) continue;
            if (!sane_sb(dec.read_bytes(sb_off, 8192))) continue;
            fs::ufs2_filesystem ufs(dec, ps);
            if (!ufs.mount()) continue;
            const auto root = ufs.read_inode(fs::ufs2_filesystem::root_inode);
            if (!root.is_directory() || root.size <= 0) continue;
            auto ents = ufs.read_directory(root);
            if (ents.size() < 2 || ents[0].name != "." || ents[1].name != "..") continue;
            if (ufs.sb().cylinder_groups > best_ncg) { best_ncg = ufs.sb().cylinder_groups; out_start = ps; }
        } catch (...) {}
    }
    return best_ncg;
}

} // namespace

std::optional<gameos_mount> open_gameos(std::shared_ptr<disk::disk_source> raw, const crypto::ata_xts_keys& xts, std::span<const std::byte> cbc) {
    const std::uint64_t sc = raw->sector_count(), ts = raw->total_size();

    struct candidate { std::shared_ptr<disk::disk_source> dec; std::string cipher; bool bswap; };
    std::vector<candidate> tries;
    for (bool bswap : {false, true})
        tries.push_back({std::make_shared<cryptodisk::encrypted_disk_source>(raw, xts.data_key, xts.tweak_key, bswap), "AES-XTS-128", bswap}); // NOR
    for (bool bswap : {false, true})
        tries.push_back({std::make_shared<cryptodisk::encrypted_disk_source_cbc>(raw, cbc, bswap), "AES-CBC-192", bswap}); // NAND

    for (auto& c : tries) {
        try {
            auto s0 = c.dec->read_bytes(0, 512);
            if (!table_magic(s0)) continue;
            std::uint64_t start = 0;
            const int ncg = best_partition(*c.dec, sc, ts, start);
            if (ncg < 0) continue;
            return gameos_mount{c.dec, start, c.cipher, c.bswap, ncg};
        } catch (...) {}
    }
    return std::nullopt;
}

std::optional<gameos_mount> open_gameos(std::shared_ptr<disk::disk_source> raw, std::span<const std::byte> eid_root_key) {
    if (eid_root_key.size() != 48) throw std::invalid_argument("EID Root Key must be 48 bytes");
    const auto xts = crypto::derive_ata_xts_keys(eid_root_key);
    const auto cbc = crypto::derive_ata_cbc_key(eid_root_key);
    return open_gameos(std::move(raw), xts, {cbc.data(), cbc.size()});
}

} // namespace ps3hdd::app