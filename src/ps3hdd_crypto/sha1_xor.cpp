#include "sha1_xor.h"

#include "be_io.h"
#include "openssl_error.h"

#include <openssl/sha.h>

#include <stdexcept>

namespace ps3hdd::crypto {

std::array<std::byte, 0x40> sha1_xor_base_key(std::span<const std::byte> qa_digest) {
    if (qa_digest.size() < 16)
        throw std::invalid_argument("sha1_xor_base_key: qa_digest must be >= 16 bytes");
    std::array<std::byte, 0x40> base{};
    std::copy_n(qa_digest.begin(), 8, base.begin() + 0x00);
    std::copy_n(qa_digest.begin(), 8, base.begin() + 0x08);
    std::copy_n(qa_digest.begin() + 8, 8, base.begin() + 0x10);
    std::copy_n(qa_digest.begin() + 8, 8, base.begin() + 0x18);
    return base;
}

void sha1_xor(std::span<const std::byte> base_key, std::uint64_t data_relative_offset, std::span<const std::byte> in, std::span<std::byte> out) {
    if (base_key.size() != 0x40)
        throw std::invalid_argument("sha1_xor: base_key must be 0x40 bytes");
    if (in.size() != out.size())
        throw std::invalid_argument("sha1_xor: input/output size mismatch");

    std::array<std::byte, 0x40> key{};
    std::copy(base_key.begin(), base_key.end(), key.begin());

    std::uint64_t counter = data_relative_offset / 16;
    int pos_in_block = static_cast<int>(data_relative_offset % 16);

    write_be_u64(key.data() + 0x38, counter);
    std::array<unsigned char, SHA_DIGEST_LENGTH> hash{};
    SHA1(reinterpret_cast<const unsigned char*>(key.data()), key.size(), hash.data());

    for (std::size_t i = 0; i < in.size(); ++i) {
        if (pos_in_block >= 16) {
            pos_in_block = 0;
            write_be_u64(key.data() + 0x38, ++counter);
            SHA1(reinterpret_cast<const unsigned char*>(key.data()), key.size(), hash.data());
        }
        out[i] = in[i] ^ static_cast<std::byte>(hash[pos_in_block]);
        ++pos_in_block;
    }
}

} // namespace ps3hdd::crypto
