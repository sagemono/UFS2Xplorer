cmake_minimum_required(VERSION 3.25)
project(ps3hddtool
    VERSION 0.9.0
    DESCRIPTION "Cross-platform PS3 HDD tool"
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

if(MSVC)
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
    add_compile_options(/W4 /permissive- /Zc:__cplusplus /Zc:preprocessor /Zc:inline /MP)
else()
    add_compile_options(-Wall -Wextra -Wpedantic)
endif()

find_package(OpenSSL REQUIRED)

option(PS3HDD_BUILD_TESTS "Build the unit tests" ON)
if(PS3HDD_BUILD_TESTS)
    find_package(Catch2 3 CONFIG REQUIRED)
    enable_testing()
endif()

add_subdirectory(src/ps3hdd_crypto)
add_subdirectory(src/ps3hdd_disk)
add_subdirectory(src/ps3hdd_fs)
add_subdirectory(src/ps3hdd_cryptodisk)
add_subdirectory(src/ps3hdd_pkg)
add_subdirectory(src/ps3hdd_license)
add_subdirectory(src/ps3hdd_mms)
add_subdirectory(src/ps3hdd_app)

option(PS3HDD_BUILD_UI "Build the Qt GUI" OFF)
if(PS3HDD_BUILD_UI)
    find_package(Qt6 REQUIRED COMPONENTS Core Widgets Network)
    qt_standard_project_setup()
    add_subdirectory(src/ps3hdd_ipc)
    add_subdirectory(src/ps3hdd_helper)
    add_subdirectory(src/ps3hdd_ui)
endif()

add_subdirectory(tools/ps3hdd_probe)
add_subdirectory(tools/ps3hdd_pkg_info)
add_subdirectory(tools/ps3hdd_install)
add_subdirectory(tools/ps3hdd_fsck)
add_subdirectory(tools/ps3hdd_ls)
add_subdirectory(tools/ps3hdd_license)
add_subdirectory(tools/ps3hdd_mmsdb)
add_subdirectory(tools/ps3hdd_import)
add_subdirectory(tools/ps3hdd_mmsinstall)

if(PS3HDD_BUILD_TESTS)
    add_subdirectory(tests)
endif()