#include "../common.h"

#include <ps3hdd_crypto/ps3_key_derivation.h>
#include <ps3hdd_cryptodisk/encrypted_disk_source.h>
#include <ps3hdd_cryptodisk/encrypted_disk_source_cbc.h>
#include <ps3hdd_disk/physical_disk_source.h>
#include <ps3hdd_disk/raw_device.h>
#include <ps3hdd_fs/ufs2_filesystem.h>

#include <cstdint>
#include <cstdio>
#include <exception>
#include <memory>
#include <set>
#include <string>
#include <vector>

using namespace ps3hdd;
using ps3hdd::tools::parse_hex;
using ps3hdd::tools::hexdump;

namespace {

std::string fmt_size(std::uint64_t b) { return disk::format_size(b); }

} // namespace

int main(int argc, char** argv) {
    const std::string path = argc > 1 ? argv[1] : "\\\\.\\PhysicalDrive3";
    std::printf("== ps3hdd_probe (READ-ONLY) ==\nDevice: %s\n\n", path.c_str());

    try {
        auto raw = disk::open_raw_device(path, /*writable=*/false);
        const auto sectors = raw->sectors();
        std::printf("[raw_device]\n");
        std::printf("  size            : %llu bytes (%s)\n", static_cast<unsigned long long>(raw->size()), fmt_size(raw->size()).c_str());
        std::printf("  logical sector  : %u\n", sectors.logical);
        std::printf("  physical sector : %u\n", sectors.physical);
        std::printf("  required align  : %u\n\n", raw->required_alignment());

        auto src = std::make_shared<disk::physical_disk_source>(std::move(raw));
        std::printf("[physical_disk_source] %s\n", src->description().c_str());
        std::printf("  total_size  : %s (%llu sectors)\n\n", fmt_size(src->total_size()).c_str(), static_cast<unsigned long long>(src->sector_count()));

        std::printf("[positioned reads]\n");
        const std::uint64_t mid = (src->sector_count() / 2) * 512;
        const std::uint64_t near_end = src->total_size() >= 4096 ? src->total_size() - 4096 : 0;
        for (auto off : {std::uint64_t{0}, mid, near_end}) {
            auto b = src->read_bytes(off, 512);
            unsigned nonzero = 0;
            for (auto x : b) if (x != std::byte{0}) ++nonzero;
            std::printf("  offset %14llu: read 512 B, %u non-zero\n", static_cast<unsigned long long>(off), nonzero);
        }
        std::printf("\n");

        std::printf("[sector 0, first 128 bytes]\n");
        hexdump(src->read_bytes(0, 512), 128);
        std::printf("\n");

        std::printf("[large read] 4 MB from offset 0 ...\n");
        auto big = src->read_sectors(0, (4u * 1024 * 1024) / 512);
        std::printf("  got %zu bytes; max_sectors_per_read now %u (%u KB)\n\n", big.size(), src->max_sectors_per_read(), src->max_sectors_per_read() * 512 / 1024);

        std::printf("[GameOS partition scan] UFS2 magic 0x19540119 expected only if decrypted\n");
        const std::uint64_t candidates[] = {0x2000, 0x4000, 0x8000, 0x10000, 0x20000};
        for (auto start_sector : candidates) {
            const std::uint64_t sb_off = start_sector * 512 + 65536;
            if (sb_off + 0x560 > src->total_size()) continue;
            auto sbmag = src->read_bytes(sb_off + 0x55C, 4);
            const std::uint32_t m = (std::to_integer<std::uint32_t>(sbmag[0]) << 24) | (std::to_integer<std::uint32_t>(sbmag[1]) << 16) | (std::to_integer<std::uint32_t>(sbmag[2]) << 8) | std::to_integer<std::uint32_t>(sbmag[3]);
            fs::ufs2_filesystem ufs(*src, start_sector);
            const bool mounted = ufs.mount();
            std::printf("  start sector 0x%05llx: magic=0x%08x mount=%s\n", static_cast<unsigned long long>(start_sector), m, mounted ? "VALID" : "no");
        }

        if (argc > 2) {
            std::printf("\n[decrypt] deriving keys from EID Root Key ...\n");
            const auto eid = parse_hex(argv[2]);
            if (eid.size() != 48) {
                std::printf("  EID Root Key must be 48 bytes; got %zu\n", eid.size());
                std::printf("\nDone.\n");
                return 0;
            }
            const auto xts = crypto::derive_ata_xts_keys(eid);
            const auto cbc = crypto::derive_ata_cbc_key(eid);

            auto be64 = [](const std::vector<std::byte>& d, std::size_t o) -> std::uint64_t {
                std::uint64_t v = 0;
                for (int i = 0; i < 8; ++i) v = (v << 8) | std::to_integer<std::uint64_t>(d[o + i]);
                return v;
            };
            auto be32 = [](const std::vector<std::byte>& d, std::size_t o) -> std::uint32_t {
                return (std::to_integer<std::uint32_t>(d[o]) << 24) | (std::to_integer<std::uint32_t>(d[o + 1]) << 16) | (std::to_integer<std::uint32_t>(d[o + 2]) << 8) | std::to_integer<std::uint32_t>(d[o + 3]);
            };

            struct cand { std::string label; std::shared_ptr<disk::disk_source> src; };
            std::vector<cand> cands;
            for (bool bs : {false, true}) // NOR
                cands.push_back({std::string("XTS-128 bswap=") + (bs ? "1" : "0"), std::make_shared<cryptodisk::encrypted_disk_source>(src, xts.data_key, xts.tweak_key, bs)});
            for (bool bs : {false, true}) // NAND
                cands.push_back({std::string("CBC-192 bswap=") + (bs ? "1" : "0"), std::make_shared<cryptodisk::encrypted_disk_source_cbc>(src, cbc, bs)});

            std::shared_ptr<disk::disk_source> dec;
            std::string used;
            for (auto& c : cands) {
                auto s0 = c.src->read_bytes(0, 512);
                const std::uint32_t m1 = be32(s0, 0x14);
                const std::uint32_t m2 = be32(s0, 0x1C);
                const bool ok = (m1 == 0x0FACE0FFu || m2 == 0xDEADFACEu);
                std::printf("  %-16s sector0 magic %08x / %08x %s\n", c.label.c_str(), m1, m2, ok ? "*** MATCH ***" : "");
                if (ok && !dec) { dec = c.src; used = c.label; }
            }
            if (!dec) {
                std::printf("  no partition table magic with any cipher, keys or layout mismatch.\n");
                std::printf("\nDone.\n");
                return 0;
            }
            std::printf("  using %s\n", used.c_str());

            auto dump = dec->read_bytes(0, 512);
            std::printf("  decrypted sector 0 (first 256 bytes):\n");
            hexdump(dump, 256);
            std::printf("  64-bit big-endian words 0x00..0xF8:\n");
            for (std::size_t o = 0; o < 0x100; o += 8) {
                const std::uint64_t v = (std::to_integer<std::uint64_t>(dump[o]) << 56) |
                    (std::to_integer<std::uint64_t>(dump[o+1]) << 48) | (std::to_integer<std::uint64_t>(dump[o+2]) << 40) |
                    (std::to_integer<std::uint64_t>(dump[o+3]) << 32) | (std::to_integer<std::uint64_t>(dump[o+4]) << 24) |
                    (std::to_integer<std::uint64_t>(dump[o+5]) << 16) | (std::to_integer<std::uint64_t>(dump[o+6]) << 8) |
                    std::to_integer<std::uint64_t>(dump[o+7]);
                if (v != 0) std::printf("    0x%02zx: %llu (0x%llx)\n", o, static_cast<unsigned long long>(v), static_cast<unsigned long long>(v));
            }

            // entries start aat 0x30 with 0x90 byte stride
            // start_sector u64, size_sectors u64
            std::vector<std::uint64_t> starts;
            auto header = dec->read_bytes(0, 2048);
            std::printf("  region table entries (0x30 + i*0x90):\n");
            for (std::size_t i = 0; i < 8; ++i) {
                const std::size_t base = 0x30 + i * 0x90;
                if (base + 16 > header.size()) break;
                const std::uint64_t pstart = be64(header, base);
                const std::uint64_t psize = be64(header, base + 8);
                if (psize == 0 || pstart >= src->sector_count()) continue;
                std::printf("    region %zu: start 0x%llx  size 0x%llx (%s)\n", i, static_cast<unsigned long long>(pstart), static_cast<unsigned long long>(psize), fmt_size(psize * 512).c_str());
                starts.push_back(pstart);
            }
            // fallback offsets in case a disk uses a layout variant
            for (auto s : {std::uint64_t{0x20}, std::uint64_t{0x800}, std::uint64_t{0x2000}, std::uint64_t{0x80010}, std::uint64_t{0x400000}})
                starts.push_back(s);

            // superblock validation
            auto is_pow2 = [](std::uint32_t v) { return v != 0 && (v & (v - 1)) == 0; };
            auto sane_geometry = [&](const std::vector<std::byte>& sb) {
                const std::uint32_t bs = be32(sb, 0x30), fsz = be32(sb, 0x34);
                const std::uint32_t ncg = be32(sb, 0x2C), ipg = be32(sb, 0xB8);
                return is_pow2(bs) && bs >= 4096 && bs <= 65536 && is_pow2(fsz) && fsz >= 512 && fsz <= bs && ncg >= 1 && ncg < 2000000 && ipg > 0;
            };

            std::uint64_t best_start = 0;
            int best_ncg = -1;
            auto validate = [&](std::uint64_t ps) {
                try {
                    const std::uint64_t sb_off = ps * 512 + 65536;
                    if (sb_off + 8192 > src->total_size()) return;
                    if (!sane_geometry(dec->read_bytes(sb_off, 8192))) return;
                    fs::ufs2_filesystem ufs(*dec, ps);
                    if (!ufs.mount()) return;
                    const auto root = ufs.read_inode(fs::ufs2_filesystem::root_inode);
                    if (!root.is_directory() || root.size <= 0 || root.size > (1 << 20)) return;
                    auto ents = ufs.read_directory(root);
                    if (ents.size() < 2 || ents[0].name != "." || ents[1].name != "..") return;
                    for (const auto& e : ents)
                        for (char c : e.name) if (c < 32 || c > 126) return;
                    const int ncg = ufs.sb().cylinder_groups;
                    std::printf("    valid root at sector 0x%llx: %zu entries, ncg=%d\n",
                                static_cast<unsigned long long>(ps), ents.size(), ncg);
                    if (ncg > best_ncg) { best_ncg = ncg; best_start = ps; }
                } catch (...) { /* not a real partition start */ }
            };

            std::printf("  validating region table candidates ...\n");
            for (auto ps : starts) validate(ps); // quick check

            if (best_ncg < 0) {
                std::printf("  no luck from the table; scanning decrypted disk ...\n");
                const std::byte magic[4] = {std::byte{0x19}, std::byte{0x54}, std::byte{0x01}, std::byte{0x19}};
                std::set<std::uint64_t> scanned;
                const std::uint64_t scan_limit = std::min<std::uint64_t>(src->total_size(), 8ull * 1024 * 1024 * 1024);
                const std::size_t chunk = 8 * 1024 * 1024, overlap = 8192;
                for (std::uint64_t pos = 0; pos < scan_limit; pos += chunk - overlap) {
                    const std::size_t want = static_cast<std::size_t>(std::min<std::uint64_t>(chunk, scan_limit - pos));
                    auto buf = dec->read_bytes(pos, want);
                    for (std::size_t j = 0; j + 4 <= buf.size(); ++j) {
                        if (buf[j] != magic[0] || buf[j+1] != magic[1] ||
                            buf[j+2] != magic[2] || buf[j+3] != magic[3]) continue;
                        const std::uint64_t global = pos + j;
                        if (global < 0x55C + 65536) continue;
                        const std::uint64_t sb_off = global - 0x55C;
                        if ((sb_off - 65536) % 512 != 0) continue;
                        scanned.insert((sb_off - 65536) / 512);
                    }
                    std::printf("    ... scanned %llu MB\r", static_cast<unsigned long long>((pos + want) / (1024 * 1024)));
                    std::fflush(stdout);
                }
                std::printf("\n");
                for (auto ps : scanned) validate(ps);
            }

            if (best_ncg < 0) {
                std::printf("  no partition with a valid root directory found.\n");
                std::printf("\nDone.\n");
                return 0;
            }

            std::printf("\n[mount] GameOS at sector 0x%llx (%d cylinder groups)\n", static_cast<unsigned long long>(best_start), best_ncg);
            fs::ufs2_filesystem ufs(*dec, best_start);
            if (ufs.mount()) {
                const auto& sb = ufs.sb();
                std::printf("  MOUNTED  bsize=%lld fsize=%lld ncg=%d free=%s vol='%s'\n", static_cast<long long>(sb.block_size), static_cast<long long>(sb.fragment_size), sb.cylinder_groups, fmt_size(static_cast<std::uint64_t>(sb.free_space_bytes())).c_str(), sb.volume_name.c_str());
                const auto root = ufs.read_inode(fs::ufs2_filesystem::root_inode);
                if (root.is_directory()) {
                    std::printf("  root directory:\n");
                    for (const auto& e : ufs.read_directory(root))
                        std::printf("    %-24s inode %u%s\n", e.name.c_str(), e.inode_number, e.type == fs::dirent_type::directory ? "/" : "");
                }

                auto list_path = [&](const std::string& p) {
                    auto node = ufs.resolve_path(p);
                    if (!node || !node->is_directory()) {
                        std::printf("  %s/: (not present)\n", p.c_str());
                        return;
                    }
                    std::printf("  %s/:\n", p.c_str());
                    int n = 0;
                    for (const auto& e : ufs.read_directory(*node)) {
                        if (e.name == "." || e.name == "..") continue;
                        std::printf("    %-44s %s\n", e.name.c_str(), e.type == fs::dirent_type::directory ? "<dir>" : "");
                        if (++n >= 40) { std::printf("    ...\n"); break; }
                    }
                    if (n == 0) std::printf("    (empty)\n");
                };
                std::printf("\n[packages] dev_hdd0/packages (PKG staging)\n");
                list_path("packages");
                std::printf("\n[game] dev_hdd0/game (installed titles)\n");
                list_path("game");
                std::printf("\n[exdata] licenses (.rif) and staged RAPs\n");
                list_path("exdata");
                if (auto home = ufs.resolve_path("home"); home && home->is_directory())
                    for (const auto& u : ufs.read_directory(*home))
                        if (u.type == fs::dirent_type::directory && u.name != "." && u.name != "..")
                            list_path("home/" + u.name + "/exdata");
            }
        }

        std::printf("\nAll reads completed without error.\n");
        return 0;
    } catch (const std::exception& e) {
        std::printf("\nERROR: %s\n", e.what());
        return 1;
    }
}