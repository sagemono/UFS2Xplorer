add_library(ps3hdd_crypto STATIC
    aes_xts_128.cpp
    aes_xts_128.h
    aes_cbc_192.cpp
    aes_cbc_192.h
    aes_ctr_128.cpp
    aes_ctr_128.h
    sha1_xor.cpp
    sha1_xor.h
    ps3_key_derivation.cpp
    ps3_key_derivation.h
    be_io.h
    bswap16.h
    openssl_error.h
)

target_include_directories(ps3hdd_crypto PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)

target_link_libraries(ps3hdd_crypto PUBLIC OpenSSL::Crypto)