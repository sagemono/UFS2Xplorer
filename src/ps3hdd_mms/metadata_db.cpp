#include "metadata_db.h"

#include <ps3hdd_crypto/be_io.h>

#include <algorithm>

namespace ps3hdd::mms {

std::uint32_t crc32_mpeg2(std::span<const std::byte> data) {
    std::uint32_t r = 0;
    for (std::byte bb : data) {
        r ^= static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(bb)) << 24;
        for (int i = 0; i < 8; ++i)
            r = (r & 0x80000000u) ? (r << 1) ^ 0x04C11DB7u : (r << 1);
    }
    return r;
}

std::uint16_t crc16_ccitt(std::span<const std::byte> data, std::uint16_t seed) {
    std::uint16_t c = seed;
    for (std::byte bb : data) {
        c ^= static_cast<std::uint16_t>(std::to_integer<std::uint8_t>(bb)) << 8;
        for (int i = 0; i < 8; ++i)
            c = (c & 0x8000u) ? (c << 1) ^ 0x1021u : (c << 1);
    }
    return c;
}

game_record game_record::parse(std::span<const std::byte> img, std::size_t base, std::size_t end) {
    game_record r;
    r.rec_len_ = end - base;
    r.fixed_.assign(img.begin() + base, img.begin() + base + kPtrTable);
    r.heap_start_ = read_be_u16(img.data() + base + kPtrTable);
    const std::size_t n = (r.heap_start_ - kPtrTable) / 4;
    for (std::size_t i = 0; i < n; ++i) {
        const std::size_t e = base + kPtrTable + i * 4;
        const std::uint16_t off = read_be_u16(img.data() + e);
        const std::uint16_t len = read_be_u16(img.data() + e + 2);
        slot s{off, len, {}, false};
        if (len > 0 && off >= r.heap_start_ && off + len <= r.rec_len_) {
            s.is_heap = true;
            s.heap.assign(img.begin() + base + off, img.begin() + base + off + len);
        }
        r.slots_.push_back(std::move(s));
    }
    return r;
}

std::vector<std::byte> game_record::serialize() const {
    std::vector<std::byte> out(fixed_);
    std::vector<const slot*> heap;
    for (const auto& s : slots_) if (s.is_heap) heap.push_back(&s);
    std::sort(heap.begin(), heap.end(), [](const slot* a, const slot* b) { return a->off < b->off; });

    std::vector<std::byte> heapbuf;
    std::vector<std::pair<const slot*, std::uint16_t>> newoff;
    std::uint16_t cur = heap_start_;
    for (const slot* s : heap) {
        newoff.emplace_back(s, cur);
        heapbuf.insert(heapbuf.end(), s->heap.begin(), s->heap.end());
        cur += static_cast<std::uint16_t>(s->heap.size());
    }
    auto off_of = [&](const slot* s) -> std::uint16_t {
        for (auto& [p, o] : newoff) if (p == s) return o;
        return 0;
    };

    for (const auto& s : slots_) {
        std::byte tmp[4];
        if (s.is_heap) {
            write_be_u16(tmp, off_of(&s));
            write_be_u16(tmp + 2, static_cast<std::uint16_t>(s.heap.size()));
        } else {
            write_be_u16(tmp, s.off);
            write_be_u16(tmp + 2, s.len);
        }
        out.insert(out.end(), tmp, tmp + 4);
    }
    out.insert(out.end(), heapbuf.begin(), heapbuf.end());
    if (out.size() < rec_len_) out.resize(rec_len_, std::byte{0});
    return out;
}

std::string game_record::get_string(std::size_t slot_idx) const {
    if (slot_idx >= slots_.size()) return {};
    const slot& s = slots_[slot_idx];
    if (!s.is_heap) return {};
    std::string out;
    for (std::byte b : s.heap) {
        if (b == std::byte{0}) break;
        out.push_back(static_cast<char>(std::to_integer<std::uint8_t>(b)));
    }
    return out;
}

void game_record::set_string(std::size_t slot_idx, std::string_view utf8) {
    slot& s = slots_[slot_idx];
    std::size_t slen = 0;
    while (slen < s.heap.size() && s.heap[slen] != std::byte{0}) ++slen;
    const std::uint8_t typecode = std::to_integer<std::uint8_t>(s.heap[slen + 4]);
    std::byte ptr[4] = {s.heap[slen + 5], s.heap[slen + 6], s.heap[slen + 7], s.heap[slen + 8]};

    std::vector<std::byte> ne;
    for (char c : utf8) ne.push_back(static_cast<std::byte>(static_cast<std::uint8_t>(c)));
    ne.insert(ne.end(), 4, std::byte{0});
    ne.push_back(static_cast<std::byte>(typecode));
    ne.insert(ne.end(), ptr, ptr + 4);
    s.heap = std::move(ne);
    s.len = static_cast<std::uint16_t>(s.heap.size());
}

void game_record::copy_scalar_block(std::span<const std::byte> other_fixed) {
    const std::size_t lo = 0xd8, hi = 0x118;
    for (std::size_t o = lo; o < hi && o < fixed_.size() && o < other_fixed.size(); ++o)
        fixed_[o] = other_fixed[o];
}

btree_leaf btree_leaf::parse(std::span<const std::byte> node) {
    btree_leaf lf;
    lf.node_.assign(node.begin(), node.begin() + kNodeSize);
    const std::uint32_t cnt = read_be_u32(lf.node_.data() + kCount);
    std::size_t p = kKeys;
    for (std::uint32_t i = 0; i < cnt && p + 4 <= lf.node_.size(); ++i) {
        const std::uint32_t len = read_be_u32(lf.node_.data() + p);
        if (len > 64 || p + 4 + len > lf.node_.size()) break;
        std::string k;
        for (std::uint32_t j = 0; j < len; ++j)
            k.push_back(static_cast<char>(std::to_integer<std::uint8_t>(lf.node_[p + 4 + j])));
        lf.keys_.push_back(std::move(k));
        p += 4 + len;
    }
    return lf;
}

bool btree_leaf::contains(std::string_view t) const {
    return std::find(keys_.begin(), keys_.end(), t) != keys_.end();
}

bool btree_leaf::insert(std::string t) {
    if (contains(t)) return false;
    keys_.push_back(std::move(t));
    std::sort(keys_.begin(), keys_.end());
    return true;
}

bool btree_leaf::remove(std::string_view t) {
    auto it = std::find(keys_.begin(), keys_.end(), t);
    if (it == keys_.end()) return false;
    keys_.erase(it);
    return true;
}

std::vector<std::byte> btree_leaf::serialize() const {
    std::vector<std::byte> out = node_;
    std::fill(out.begin() + kKeys, out.begin() + kNodeSize, std::byte{0});
    write_be_u32(out.data() + kCount, count());
    std::size_t p = kKeys;
    for (const auto& k : keys_) {
        write_be_u32(out.data() + p, static_cast<std::uint32_t>(k.size()));
        for (std::size_t j = 0; j < k.size(); ++j)
            out[p + 4 + j] = static_cast<std::byte>(static_cast<std::uint8_t>(k[j]));
        p += 4 + k.size();
    }
    for (std::size_t i = 0; i < keys_.size(); ++i) {
        write_be_u32(out.data() + p, static_cast<std::uint32_t>(4 + i * 0xC));
        write_be_u32(out.data() + p + 4, 4);
        p += 8;
    }
    const std::uint32_t c = crc32_mpeg2(std::span<const std::byte>(out).subspan(kUsed, kCrc - kUsed));
    write_be_u32(out.data() + kCrc, c);
    return out;
}

bool verify_idx_crcs(std::span<const std::byte> idx, std::size_t* bad_offset) {
    if (idx.size() < 0xB0) return false;
    if (crc32_mpeg2(idx.subspan(0, 0xAC)) != read_be_u32(idx.data() + 0xAC)) {
        if (bad_offset) *bad_offset = 0;
        return false;
    }
    for (std::size_t base = kNodeSize; base + kNodeSize <= idx.size(); base += kNodeSize) {
        auto node = idx.subspan(base, kNodeSize);
        bool all_zero = std::all_of(node.begin(), node.end(), [](std::byte b) { return b == std::byte{0}; });
        if (all_zero) continue;
        const std::uint32_t stored = read_be_u32(idx.data() + base + 0xC);
        const std::uint32_t calc = crc32_mpeg2(node.subspan(0x10));
        if (calc == stored) continue;
    }
    return true;
}

namespace {
std::string read_cstr(std::span<const std::byte> d, std::size_t off, std::size_t max) {
    std::string s;
    for (std::size_t i = 0; i < max && off + i < d.size(); ++i) {
        const auto c = std::to_integer<std::uint8_t>(d[off + i]);
        if (c == 0) break;
        s.push_back(static_cast<char>(c));
    }
    return s;
}

std::string read_field(std::span<const std::byte> d, std::size_t off, std::size_t width) {
    std::string s;
    for (std::size_t i = 0; i < width && off + i < d.size(); ++i) {
        const auto c = std::to_integer<std::uint8_t>(d[off + i]);
        if (c >= 0x20 && c < 0x7f) s.push_back(static_cast<char>(c));
        else if (!s.empty()) break;
    }
    return s;
}

bool is_titleid_char(std::uint8_t c) { return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'); }

} // namespace

std::vector<game_info> enumerate_games(std::span<const std::byte> d) {
    std::vector<game_info> out;
    std::vector<bool> seen;
    for (std::size_t i = 0; i + 10 < d.size(); ++i) {
        auto at = [&](std::size_t k) { return std::to_integer<std::uint8_t>(d[i + k]); };
        if (!(at(0) >= 'A' && at(0) <= 'Z' && at(1) >= 'A' && at(1) <= 'Z')) continue;
        if (!is_titleid_char(at(2)) || !is_titleid_char(at(3))) continue;
        bool digits = true;
        for (int k = 4; k < 9; ++k) if (!(at(k) >= '0' && at(k) <= '9')) digits = false;
        if (!digits || at(9) != 0) continue;
        const std::size_t tid = i;
        const std::size_t cat = tid + 0x38;
        if (cat + 1 >= d.size()) continue;
        const auto c0 = std::to_integer<std::uint8_t>(d[cat]);
        const auto c1 = std::to_integer<std::uint8_t>(d[cat + 1]);
        if (!(c0 >= 'A' && c0 <= 'Z' && c1 >= 'A' && c1 <= 'Z')) continue;
        if (tid < 0xd9) continue;
        const std::size_t base = tid - 0xd9;

        game_info g;
        g.offset = base;
        g.title_id = read_cstr(d, tid, 12);
        g.category = read_field(d, base + 0x110, 4);
        g.version = read_field(d, base + 0xf8, 8);
        g.ps3_system_ver = read_field(d, base + 0x108, 8);
        try {
            const std::size_t end = std::min(base + 0x1000, d.size());
            auto rec = game_record::parse(d, base, end);
            if (rec.slot_count() > 4) { g.title = rec.get_string(1); g.dir_path = rec.get_string(4); }
        } catch (...) {}
        out.push_back(std::move(g));
        i = tid + 9;
    }
    return out;
}

db_report analyze(std::span<const std::byte> container, std::span<const std::byte> idx) {
    db_report r;
    r.container_magic_ok = container.size() >= 8 &&
        std::equal(kContainerMagic.begin(), kContainerMagic.end(), container.begin());
    r.idx_crcs_ok = verify_idx_crcs(idx, &r.idx_bad_offset);
    for (std::size_t base = 0; base + kNodeSize <= idx.size(); base += kNodeSize) {
        if (std::to_integer<std::uint8_t>(idx[base + 0x800]) == 0x03) {
            ++r.index_trees;
            r.indexed_keys += static_cast<int>(read_be_u32(idx.data() + base + 0x804));
        }
    }
    static constexpr std::array<std::byte, 8> kBTreeMagic = {
        std::byte{'M'}, std::byte{'m'}, std::byte{'s'}, std::byte{'B'},
        std::byte{'T'}, std::byte{'r'}, std::byte{'e'}, std::byte{'e'}};
    for (std::size_t i = 8; i + 8 <= container.size(); ++i) {
        if (std::equal(kBTreeMagic.begin(), kBTreeMagic.end(), container.begin() + i)) {
            r.master_index_offset = i;
            break;
        }
    }
    r.games = enumerate_games(container);
    return r;
}

} // namespace ps3hdd::mms