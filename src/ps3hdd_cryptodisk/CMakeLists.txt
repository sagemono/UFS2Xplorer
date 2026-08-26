add_library(ps3hdd_cryptodisk STATIC
    encrypted_disk_source.cpp
    encrypted_disk_source.h
    encrypted_disk_source_cbc.cpp
    encrypted_disk_source_cbc.h
)

target_include_directories(ps3hdd_cryptodisk PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(ps3hdd_cryptodisk PUBLIC ps3hdd_disk ps3hdd_crypto)