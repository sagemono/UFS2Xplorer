#include "activation.h"

#include "ecdsa.h"

#include <openssl/evp.h>

#include <cstdint>
#include <cstring>
#include <stdexcept>

namespace ps3hdd::license {

namespace {

using u8 = std::uint8_t;

constexpr u8 npdrm_const_key[16] = {
    0x5E, 0x06, 0xE0, 0x4F, 0xD9, 0x4A, 0x71, 0xBF,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01
};
constexpr u8 npdrm_rif_key[16] = {
    0xDA, 0x7D, 0x4B, 0x5E, 0x49, 0x9A, 0x4F, 0x53,
    0xB1, 0xC1, 0xA1, 0x4A, 0x74, 0x84, 0x44, 0x3B
};

void aes128_ecb(const u8 key[16], const u8 in[16], u8 out[16], bool encrypt) {
    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (!ctx) throw std::runtime_error("EVP_CIPHER_CTX_new failed");
    int len = 0;
    const int ok =
        (encrypt ? EVP_EncryptInit_ex(ctx, EVP_aes_128_ecb(), nullptr, key, nullptr) : EVP_DecryptInit_ex(ctx, EVP_aes_128_ecb(), nullptr, key, nullptr)) == 1 &&
        EVP_CIPHER_CTX_set_padding(ctx, 0) == 1 &&
        (encrypt ? EVP_EncryptUpdate(ctx, out, &len, in, 16) : EVP_DecryptUpdate(ctx, out, &len, in, 16)) == 1;
    EVP_CIPHER_CTX_free(ctx);
    if (!ok) throw std::runtime_error("AES-128-ECB failed");
}
void aes_enc(const u8 k[16], const u8 in[16], u8 out[16]) { aes128_ecb(k, in, out, true); }
void aes_dec(const u8 k[16], const u8 in[16], u8 out[16]) { aes128_ecb(k, in, out, false); }

void sha1(const u8* data, std::size_t len, u8 out[20]) {
    unsigned int n = 0;
    if (EVP_Digest(data, len, out, &n, EVP_sha1(), nullptr) != 1 || n != 20)
        throw std::runtime_error("SHA1 failed");
}

void put_be32(u8* p, std::uint32_t v) {
    p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v;
}
void put_be64(u8* p, std::uint64_t v) {
    for (int i = 0; i < 8; ++i) p[i] = static_cast<u8>(v >> (56 - 8 * i));
}

// act.dat field offsets
constexpr std::size_t ACT_SIZE = 0x1038;
constexpr std::size_t ACT_KEYTABLE = 0x10; // 0x800

// rif field offsets
constexpr std::size_t RIF_SIZE = 0x98;
constexpr std::size_t RIF_TITLEID = 0x10; // 0x30
constexpr std::size_t RIF_PADDING = 0x40; // 0xC padding + 0x4 actDatIndex = one AES block
constexpr std::size_t RIF_KEY = 0x50; // 0x10 encrypted klicensee
constexpr std::size_t RIF_R = 0x70; // 0x14
constexpr std::size_t RIF_S = 0x84; // 0x14


void assemble_rif(const u8 idps_b[16], const std::string& content_id, const u8 klic_b[16], const u8 acct[8], const u8 keytable0[16], u8* rif) {
    std::memset(rif, 0, RIF_SIZE);
    put_be32(rif + 0x00, 1);          // version
    put_be32(rif + 0x04, 0x00010002); // licenseType
    std::memcpy(rif + 0x08, acct, 8); // accountid (exact bytes)
    std::memcpy(rif + RIF_TITLEID, content_id.data(), content_id.size());
    // padding[0xC] + actDatIndex(u32) stay zero (index 0)
    put_be64(rif + 0x60, 0x0000012F415C0000ull); // timestamp; expiration @0x68 = 0

    u8 idps_const[16], act_key[16];
    aes_enc(idps_b, npdrm_const_key, idps_const);
    aes_dec(idps_const, keytable0, act_key);
    aes_enc(act_key, klic_b, rif + RIF_KEY);
    aes_enc(npdrm_rif_key, rif + RIF_PADDING, rif + RIF_PADDING);

    u8 h[20];
    sha1(rif, RIF_R, h);
    u8 R[21], S[21];
    ps3_ecdsa_sign(h, R, S);
    std::memcpy(rif + RIF_R, R + 1, 0x14);
    std::memcpy(rif + RIF_S, S + 1, 0x14);
}

} // namespace

activation build_activation(std::span<const std::byte> idps, const std::string& content_id, std::span<const std::byte> klicensee, std::uint64_t account_id) {
    if (idps.size() != 16) throw std::invalid_argument("IDPS must be 16 bytes");
    if (klicensee.size() != 16) throw std::invalid_argument("klicensee must be 16 bytes");
    if (content_id.size() > 0x30) throw std::invalid_argument("content id too long");

    u8 idps_b[16], klic_b[16];
    for (int i = 0; i < 16; ++i) idps_b[i] = std::to_integer<u8>(idps[i]);
    for (int i = 0; i < 16; ++i) klic_b[i] = std::to_integer<u8>(klicensee[i]);

    activation out{};
    u8* act = reinterpret_cast<u8*>(out.act_dat.data());
    u8* rif = reinterpret_cast<u8*>(out.rif.data());
    std::memset(act, 0x11, ACT_SIZE);
    put_be32(act + 0x00, 0x00000001); // version
    put_be32(act + 0x04, 0x00000002); // licenseType
    put_be64(act + 0x08, account_id); // accountId in BE
    static const u8 timedata[16] = {0x00, 0x00, 0x01, 0x2F, 0x3F, 0xFF, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    std::memcpy(act + 0x870, timedata, sizeof timedata);

    u8 acct[8];
    put_be64(acct, account_id);
    assemble_rif(idps_b, content_id, klic_b, acct, act + ACT_KEYTABLE, rif);
    return out;
}

std::array<std::byte, 0x98> build_rif(std::span<const std::byte> idps, const std::string& content_id, std::span<const std::byte> klicensee, std::span<const std::byte> act_dat) {
    if (idps.size() != 16) throw std::invalid_argument("IDPS must be 16 bytes");
    if (klicensee.size() != 16) throw std::invalid_argument("klicensee must be 16 bytes");
    if (content_id.size() > 0x30) throw std::invalid_argument("content id too long");
    if (act_dat.size() != ACT_SIZE) throw std::invalid_argument("bad act.dat size");

    u8 idps_b[16], klic_b[16], acct[8], keytable0[16];
    for (int i = 0; i < 16; ++i) idps_b[i] = std::to_integer<u8>(idps[i]);
    for (int i = 0; i < 16; ++i) klic_b[i] = std::to_integer<u8>(klicensee[i]);
    for (int i = 0; i < 8; ++i) acct[i] = std::to_integer<u8>(act_dat[0x08 + i]);
    for (int i = 0; i < 16; ++i) keytable0[i] = std::to_integer<u8>(act_dat[ACT_KEYTABLE + i]);

    std::array<std::byte, 0x98> out{};
    assemble_rif(idps_b, content_id, klic_b, acct, keytable0, reinterpret_cast<u8*>(out.data()));
    return out;
}

std::array<std::byte, 16> rif_recover_klicensee(std::span<const std::byte> idps, std::span<const std::byte> act_dat, std::span<const std::byte> rif) {
    if (idps.size() != 16) throw std::invalid_argument("IDPS must be 16 bytes");
    if (act_dat.size() != ACT_SIZE) throw std::invalid_argument("bad act.dat size");
    if (rif.size() != RIF_SIZE) throw std::invalid_argument("bad rif size");

    u8 idps_b[16], act[ACT_SIZE], r[RIF_SIZE];
    for (int i = 0; i < 16; ++i) idps_b[i] = std::to_integer<u8>(idps[i]);
    for (std::size_t i = 0; i < ACT_SIZE; ++i) act[i] = std::to_integer<u8>(act_dat[i]);
    for (std::size_t i = 0; i < RIF_SIZE; ++i) r[i] = std::to_integer<u8>(rif[i]);

    // now undo the paddingblock encryption to recover actDatIndex
    u8 pad[16];
    aes_dec(npdrm_rif_key, r + RIF_PADDING, pad);
    const std::uint32_t act_index =
        (pad[0xC] << 24) | (pad[0xD] << 16) | (pad[0xE] << 8) | pad[0xF];

    u8 idps_const[16], act_key[16], klic[16];
    aes_enc(idps_b, npdrm_const_key, idps_const);
    aes_dec(idps_const, act + ACT_KEYTABLE + act_index * 0x10, act_key);
    aes_dec(act_key, r + RIF_KEY, klic);

    std::array<std::byte, 16> out{};
    for (int i = 0; i < 16; ++i) out[i] = static_cast<std::byte>(klic[i]);
    return out;
}

} // namespace ps3hdd::license