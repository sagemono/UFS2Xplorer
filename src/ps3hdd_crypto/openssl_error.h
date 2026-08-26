#pragma once

#include <openssl/err.h>

#include <stdexcept>
#include <string>

namespace ps3hdd::crypto {

class crypto_error : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

[[noreturn]] inline void throw_openssl(const std::string& what) {
    char buf[256] = {0};
    const unsigned long e = ERR_get_error();
    if (e != 0) ERR_error_string_n(e, buf, sizeof buf);
    throw crypto_error(what + (e ? std::string(": ") + buf : std::string()));
}

} // namespace ps3hdd::crypto