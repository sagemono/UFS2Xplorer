add_executable(ps3hdd_crypto_tests
    test_aes_xts_128.cpp
    test_aes_cbc_192.cpp
    test_aes_ctr_128.cpp
    test_sha1_xor.cpp
)

target_link_libraries(ps3hdd_crypto_tests PRIVATE
    ps3hdd_crypto
    OpenSSL::Crypto
    Catch2::Catch2WithMain
)

include(Catch)
catch_discover_tests(ps3hdd_crypto_tests)

add_executable(ps3hdd_disk_tests
    test_image_disk_source.cpp
    test_physical_disk_source.cpp
)

target_link_libraries(ps3hdd_disk_tests PRIVATE
    ps3hdd_disk
    Catch2::Catch2WithMain
)

catch_discover_tests(ps3hdd_disk_tests)

add_executable(ps3hdd_fs_tests
    test_ufs2_parse.cpp
    test_ufs2_filesystem.cpp
    test_ufs2_writer.cpp
)

target_link_libraries(ps3hdd_fs_tests PRIVATE
    ps3hdd_fs
    Catch2::Catch2WithMain
)

catch_discover_tests(ps3hdd_fs_tests)

add_executable(ps3hdd_cryptodisk_tests
    test_encrypted_disk_source.cpp
)

target_link_libraries(ps3hdd_cryptodisk_tests PRIVATE
    ps3hdd_cryptodisk
    Catch2::Catch2WithMain
)

catch_discover_tests(ps3hdd_cryptodisk_tests)

add_executable(ps3hdd_pkg_tests
    test_pkg_install.cpp
)

target_link_libraries(ps3hdd_pkg_tests PRIVATE
    ps3hdd_pkg
    ps3hdd_cryptodisk
    Catch2::Catch2WithMain
)

catch_discover_tests(ps3hdd_pkg_tests)

add_executable(ps3hdd_license_tests
    test_rap.cpp
    test_activation.cpp
)

target_link_libraries(ps3hdd_license_tests PRIVATE
    ps3hdd_license
    OpenSSL::Crypto
    Catch2::Catch2WithMain
)

catch_discover_tests(ps3hdd_license_tests)

add_executable(ps3hdd_mms_tests
    test_metadata_db.cpp
)

target_link_libraries(ps3hdd_mms_tests PRIVATE
    ps3hdd_mms
    Catch2::Catch2WithMain
)

catch_discover_tests(ps3hdd_mms_tests)
