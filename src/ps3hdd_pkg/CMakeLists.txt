add_library(ps3hdd_pkg STATIC
    ps3_pkg_reader.cpp
    ps3_pkg_reader.h
    pkg_installer.cpp
    pkg_installer.h
)

target_include_directories(ps3hdd_pkg PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(ps3hdd_pkg PUBLIC ps3hdd_fs ps3hdd_crypto)