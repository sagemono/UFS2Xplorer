#define OPENSSL_SUPPRESS_DEPRECATED

#include "ecdsa.h"

#include <openssl/bn.h>
#include <openssl/ec.h>
#include <openssl/ecdsa.h>

#include <cstring>
#include <memory>

namespace ps3hdd::license {

namespace {

using u8 = std::uint8_t;

const u8 kP[20]  = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0x00,0x00,0x00,0x01,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};
const u8 kA[20]  = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0x00,0x00,0x00,0x01,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFC};
const u8 kB[20]  = {0xA6,0x8B,0xED,0xC3,0x34,0x18,0x02,0x9C,0x1D,0x3C,0xE3,0x3B,0x9A,0x32,0x1F,0xCC,0xBB,0x9E,0x0F,0x0B};
const u8 kN[21]  = {0x00,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFE,0xFF,0xFF,0xB5,0xAE,0x3C,0x52,0x3E,0x63,0x94,0x4F,0x21,0x27};
const u8 kGx[20] = {0x12,0x8E,0xC4,0x25,0x64,0x87,0xFD,0x8F,0xDF,0x64,0xE2,0x43,0x7B,0xC0,0xA1,0xF6,0xD5,0xAF,0xDE,0x2C};
const u8 kGy[20] = {0x59,0x58,0x55,0x7E,0xB1,0xDB,0x00,0x12,0x60,0x42,0x55,0x24,0xDB,0xC3,0x79,0xD5,0xAC,0x5F,0x4A,0xDF};

const u8 kNpdrmPriv[21] = {0x00,0xbf,0x21,0x22,0x4b,0x04,0x1f,0x29,0x54,0x9d,0xb2,0x5e,0x9a,0xad,0xe1,0x9e,0x72,0x0a,0x1f,0xe0,0xf1};
const u8 kNpdrmPub[40] = {0x94,0x8D,0xA1,0x3E,0x8C,0xAF,0xD5,0xBA,0x0E,0x90,0xCE,0x43,0x44,0x61,0xBB,0x32,0x7F,0xE7,0xE0,0x80,0x47,0x5E,0xAA,0x0A,0xD3,0xAD,0x4F,0x5B,0x62,0x47,0xA7,0xFD,0xA8,0x6D,0xF6,0x97,0x90,0x19,0x67,0x73};

const u8 kPkgPub[40] = {0xE6,0x79,0x2E,0x44,0x6C,0xEB,0xA2,0x7B,0xCA,0xDF,0x37,0x4B,0x99,0x50,0x4F,0xD8,0xE8,0x0A,0xDF,0xEB,0x3E,0x66,0xDE,0x73,0xFF,0xE5,0x8D,0x32,0x91,0x22,0x1C,0x65,0x01,0x8C,0x03,0x8D,0x38,0x22,0xC3,0xC9};

struct Free {
    void operator()(BIGNUM* p) const { BN_free(p); }
    void operator()(BN_CTX* p) const { BN_CTX_free(p); }
    void operator()(EC_GROUP* p) const { EC_GROUP_free(p); }
    void operator()(EC_POINT* p) const { EC_POINT_free(p); }
    void operator()(EC_KEY* p) const { EC_KEY_free(p); }
    void operator()(ECDSA_SIG* p) const { ECDSA_SIG_free(p); }
};
template <class T> using P = std::unique_ptr<T, Free>;
P<BIGNUM> bn(const u8* d, int n) { return P<BIGNUM>(BN_bin2bn(d, n, nullptr)); }

P<EC_GROUP> new_curve(BN_CTX* ctx) {
    auto p = bn(kP, 20), a = bn(kA, 20), b = bn(kB, 20);
    P<EC_GROUP> g(EC_GROUP_new_curve_GFp(p.get(), a.get(), b.get(), ctx));
    if (!g) return nullptr;
    auto gx = bn(kGx, 20), gy = bn(kGy, 20), n = bn(kN, 21);
    P<EC_POINT> G(EC_POINT_new(g.get()));
    P<BIGNUM> h(BN_new());
    BN_one(h.get()); // cofactor 1
    if (!G || !h || EC_POINT_set_affine_coordinates(g.get(), G.get(), gx.get(), gy.get(), ctx) != 1 ||
        EC_GROUP_set_generator(g.get(), G.get(), n.get(), h.get()) != 1)
        return nullptr;
    return g;
}

bool verify_with(const u8 pub40[40], const u8 hash[20], const u8 r[21], const u8 s[21]) {
    P<BN_CTX> ctx(BN_CTX_new());
    P<EC_GROUP> g = new_curve(ctx.get());
    if (!ctx || !g) return false;
    P<EC_KEY> key(EC_KEY_new());
    if (!key || EC_KEY_set_group(key.get(), g.get()) != 1) return false;
    auto qx = bn(pub40, 20), qy = bn(pub40 + 20, 20);
    P<EC_POINT> Q(EC_POINT_new(g.get()));
    if (!Q || EC_POINT_set_affine_coordinates(g.get(), Q.get(), qx.get(), qy.get(), ctx.get()) != 1 ||
        EC_KEY_set_public_key(key.get(), Q.get()) != 1)
        return false;
    P<ECDSA_SIG> sig(ECDSA_SIG_new());
    if (!sig) return false;
    BIGNUM* rr = BN_bin2bn(r, 21, nullptr);
    BIGNUM* ss = BN_bin2bn(s, 21, nullptr);
    if (!rr || !ss || ECDSA_SIG_set0(sig.get(), rr, ss) != 1) {
        BN_free(rr);
        BN_free(ss);
        return false;
    }
    return ECDSA_do_verify(hash, 20, sig.get(), key.get()) == 1;
}

} // namespace

void ps3_ecdsa_sign(const std::uint8_t hash[20], std::uint8_t r[21], std::uint8_t s[21]) {
    std::memset(r, 0, 21);
    std::memset(s, 0, 21);
    P<BN_CTX> ctx(BN_CTX_new());
    P<EC_GROUP> g = new_curve(ctx.get());
    if (!ctx || !g) return;
    P<EC_KEY> key(EC_KEY_new());
    auto priv = bn(kNpdrmPriv, 21);
    P<EC_POINT> pub(EC_POINT_new(g.get()));
    if (!key || !pub || EC_KEY_set_group(key.get(), g.get()) != 1 ||
        EC_KEY_set_private_key(key.get(), priv.get()) != 1 ||
        EC_POINT_mul(g.get(), pub.get(), priv.get(), nullptr, nullptr, ctx.get()) != 1 ||
        EC_KEY_set_public_key(key.get(), pub.get()) != 1)
        return;
    P<ECDSA_SIG> sig(ECDSA_do_sign(hash, 20, key.get()));
    if (!sig) return;
    const BIGNUM* rr = nullptr;
    const BIGNUM* ss = nullptr;
    ECDSA_SIG_get0(sig.get(), &rr, &ss);
    BN_bn2binpad(rr, r, 21);
    BN_bn2binpad(ss, s, 21);
}

bool ps3_ecdsa_verify(const std::uint8_t hash[20], const std::uint8_t r[21], const std::uint8_t s[21]) {
    return verify_with(kNpdrmPub, hash, r, s);
}

bool ps3_pkg_ecdsa_verify(const std::uint8_t hash[20], const std::uint8_t r[21], const std::uint8_t s[21]) {
    return verify_with(kPkgPub, hash, r, s);
}

} // namespace ps3hdd::license