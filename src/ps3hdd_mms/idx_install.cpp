#include "idx_install.h"

#include "metadata_db.h" // crc32_mpeg2
#include <ps3hdd_crypto/be_io.h>

#include <algorithm>
#include <array>
#include <stdexcept>

namespace ps3hdd::mms {
namespace {

constexpr std::size_t kSub = 0x400;

struct leaf_view {
    std::vector<std::string> keys;
    std::vector<std::uint32_t> nobjs;
};

leaf_view parse_leaf(std::span<const std::byte> leaf) {
    leaf_view v;
    const std::uint32_t cnt = read_be_u32(leaf.data() + 4);
    std::size_t p = 0x20;
    for (std::uint32_t i = 0; i < cnt; ++i) {
        const std::uint32_t len = read_be_u32(leaf.data() + p);
        std::string k;
        for (std::uint32_t j = 0; j < len; ++j)
            k.push_back(static_cast<char>(std::to_integer<std::uint8_t>(leaf[p + 4 + j])));
        v.keys.push_back(std::move(k));
        p += 4 + len;
    }
    for (std::uint32_t i = 0; i < cnt; ++i)
        v.nobjs.push_back(read_be_u32(leaf.data() + p + i * 8 + 4) / 4);
    return v;
}

std::uint32_t reg(std::uint32_t n) { return 4 + 8 * n; }

std::vector<std::byte> emit_leaf(std::span<const std::byte> hdr20, const std::vector<std::string>& keys, const std::vector<std::uint32_t>& nobjs, bool inl) {
    std::vector<std::byte> out(kSub, std::byte{0});
    std::copy(hdr20.begin(), hdr20.begin() + 0x20, out.begin());
    write_be_u32(out.data() + 4, static_cast<std::uint32_t>(keys.size()));
    std::size_t p = 0x20;
    for (const auto& k : keys) {
        write_be_u32(out.data() + p, static_cast<std::uint32_t>(k.size()));
        for (std::size_t j = 0; j < k.size(); ++j)
            out[p + 4 + j] = static_cast<std::byte>(static_cast<std::uint8_t>(k[j]));
        p += 4 + k.size();
    }
    std::uint32_t off = 4;
    for (std::uint32_t n : nobjs) {
        write_be_u32(out.data() + p, off);
        write_be_u32(out.data() + p + 4, 4 * n);
        p += 8;
        off += reg(n);
    }
    if (inl) {
        const bool single = nobjs.size() == 1;
        const std::uint32_t base = single ? 4 : 0x10;
        for (std::uint32_t n : nobjs) {
            if (n < 2) continue;
            if (single) { write_be_u32(out.data() + p, 0); p += 4; }
            write_be_u32(out.data() + p, base);
            write_be_u32(out.data() + p + 4, 4 * (n - 1));
            p += 8;
            write_be_u32(out.data() + p, base + ssize(n - 1));
            write_be_u32(out.data() + p + 4, 4);
            p += 8;
        }
    }
    const std::uint32_t c = crc32_mpeg2(std::span<const std::byte>(out).subspan(0, 0x1c));
    write_be_u32(out.data() + 0x1c, c);
    return out;
}

} // namespace

std::uint32_t ssize(std::uint32_t m) {
    if (m == 0) return 0;
    std::uint32_t p = 1;
    while (p * 2 <= m) p *= 2;
    return 4 + 8 * p;
}

std::vector<std::uint32_t> tree_trailer(std::uint32_t n) {
    std::uint32_t P = 1;
    while (P * 2 < n) P *= 2;
    std::vector<std::uint32_t> full;
    if (P >= 2) { full.push_back(P - 1); for (std::uint32_t i = 0; i < P - 2; ++i) full.push_back(0); }
    const std::uint32_t k = (n > P + 1) ? n - (P + 1) : 0;
    std::vector<std::uint32_t> out(full.begin() + std::min<std::size_t>(k, full.size()), full.end());
    out.push_back(4);
    out.push_back(n - 1);
    return out;
}

std::vector<std::uint32_t> shed_trailer(std::uint32_t n) {
    std::uint32_t P = 1;
    while (P * 2 <= n) P *= 2;
    std::vector<std::uint32_t> full;
    full.push_back(4);
    for (std::uint32_t i = 0; i < P - 1; ++i) full.push_back(0);
    full.push_back(4);
    return std::vector<std::uint32_t>(full.begin() + (n - P), full.end());
}

std::vector<std::byte> gen_leaf_add(std::span<const std::byte> leaf, const std::string& key, bool inl) {
    leaf_view v = parse_leaf(leaf);
    v.keys.push_back(key);
    v.nobjs.push_back(1);
    std::vector<std::size_t> idx(v.keys.size());
    for (std::size_t i = 0; i < idx.size(); ++i) idx[i] = i;
    std::sort(idx.begin(), idx.end(), [&](std::size_t a, std::size_t b) { return v.keys[a] < v.keys[b]; });
    std::vector<std::string> nk; std::vector<std::uint32_t> nn;
    for (std::size_t i : idx) { nk.push_back(v.keys[i]); nn.push_back(v.nobjs[i]); }
    return emit_leaf(leaf.subspan(0, 0x20), nk, nn, inl);
}

std::vector<std::byte> gen_leaf_bump(std::span<const std::byte> leaf, const std::string& key, bool inl) {
    leaf_view v = parse_leaf(leaf);
    auto it = std::find(v.keys.begin(), v.keys.end(), key);
    if (it == v.keys.end()) throw std::runtime_error("gen_leaf_bump: key not present");
    v.nobjs[static_cast<std::size_t>(it - v.keys.begin())] += 1;
    return emit_leaf(leaf.subspan(0, 0x20), v.keys, v.nobjs, inl);
}

std::vector<std::byte> gen_objref(std::span<const std::byte> block, objref_family fam, std::uint32_t new_obj) {
    std::vector<std::byte> out(block.begin(), block.end());
    const std::uint32_t n = read_be_u32(block.data()) / 4;

    if (fam == objref_family::append_entry) {
        std::size_t end = kSub;
        while (end > 0 && block[end - 1] == std::byte{0}) --end;
        write_be_u32(out.data() + end, 0);
        write_be_u32(out.data() + end + 4, 4);
        write_be_u32(out.data() + end + 8, new_obj);
        return out;
    }

    std::vector<std::uint32_t> lst;
    for (std::uint32_t i = 0; i < n; ++i) lst.push_back(read_be_u32(block.data() + 4 + i * 4));
    std::fill(out.begin(), out.end(), std::byte{0});

    std::vector<std::uint32_t> lst2 = lst;
    std::vector<std::uint32_t> tr;
    if (fam == objref_family::flat) {
        lst2.push_back(new_obj);
    } else {
        const bool zero_indexed = !lst.empty() && lst.front() == 0;
        lst2.push_back(zero_indexed ? static_cast<std::uint32_t>(lst.size()) : static_cast<std::uint32_t>(lst.size()) + 1);
        tr = (fam == objref_family::tree) ? tree_trailer(static_cast<std::uint32_t>(lst2.size())) : shed_trailer(static_cast<std::uint32_t>(lst2.size()));
    }
    write_be_u32(out.data(), static_cast<std::uint32_t>(lst2.size()) * 4);
    std::size_t p = 4;
    for (std::uint32_t v : lst2) { write_be_u32(out.data() + p, v); p += 4; }
    for (std::uint32_t v : tr)   { write_be_u32(out.data() + p, v); p += 4; }
    return out;
}

std::vector<std::byte> install_into_idx(std::span<const std::byte> idx, const install_keys& k, std::uint32_t new_obj) {
    std::vector<std::byte> out(idx.begin(), idx.end());
    auto sub = [&](std::size_t off) { return std::span<const std::byte>(out).subspan(off, kSub); };
    auto put = [&](std::size_t off, const std::vector<std::byte>& b) {
        std::copy(b.begin(), b.end(), out.begin() + off);
    };

    put(0x37800, gen_leaf_add(sub(0x37800), k.title_id, false));
    put(0x35000, gen_leaf_add(sub(0x35000), k.title_sort, false));
    put(0x2dc00, gen_leaf_bump(sub(0x2dc00), k.date, false));
    put(0x2f400, gen_leaf_bump(sub(0x2f400), k.date, true));
    put(0x2e800, gen_leaf_bump(sub(0x2e800), k.owner, true));
    put(0x31c00, gen_leaf_bump(sub(0x31c00), k.status, false));
    put(0x3a000, gen_leaf_bump(sub(0x3a000), k.dir_path, false));
    put(0x3ec00, gen_leaf_bump(sub(0x3ec00), k.ff, false));

    struct { std::size_t off; objref_family fam; } refs[] = {
        {0x2fc00, objref_family::flat},  {0x38000, objref_family::flat},
        {0x3cc00, objref_family::flat},  {0x2e400, objref_family::tree},
        {0x2bc00, objref_family::shed},  {0x2f000, objref_family::shed},
        {0x34c00, objref_family::append_entry}, {0x35800, objref_family::append_entry},
    };
    for (auto& r : refs) put(r.off, gen_objref(sub(r.off), r.fam, new_obj));
    return out;
}

std::vector<std::byte> install_into_container(std::span<const std::byte> container, std::uint32_t new_obj, const container_write& w) {
    std::vector<std::byte> out(container.begin(), container.end());
    auto u32 = [&](std::size_t o) { return read_be_u32(out.data() + o); };
    auto add = [&](std::size_t o, std::uint32_t d) { write_be_u32(out.data() + o, u32(o) + d); };

    std::copy(w.record.begin(), w.record.end(), out.begin() + w.record_at);

    add(0x16004, static_cast<std::uint32_t>(-static_cast<std::int32_t>(w.heap_alloc)));  // free ptr -=
    add(0x16010, w.heap_alloc); // heap-used +=
    add(0x16018, 1); // object count +1
    write_be_u32(out.data() + w.objentry_at + 0, new_obj);
    write_be_u32(out.data() + w.objentry_at + 4, w.timestamp);
    write_be_u32(out.data() + w.objentry_at + 8, 0);
    write_be_u32(out.data() + w.objentry_at + 12, w.heap_base);
    write_be_u32(out.data() + w.objentry_at + 16, w.obj_off);

    struct { std::size_t changed; std::uint32_t delta; } ctr[] = {
        {0x209f, 2}, {0x14033, 1}, {0x1403b, 1}, {0x14057, 5},
        {0x1a03f, 4}, {0x1c1d4, 1}, {0x1c3c0, 1},
    };
    for (auto& c : ctr) add(c.changed - 3, c.delta);
    return out;
}

} // namespace ps3hdd::mms