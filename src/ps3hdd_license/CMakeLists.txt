add_library(ps3hdd_license STATIC
    rap.cpp
    rap.h
    ecdsa.cpp
    ecdsa.h
    activation.cpp
    activation.h
)

target_include_directories(ps3hdd_license PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)

target_link_libraries(ps3hdd_license PUBLIC OpenSSL::Crypto)