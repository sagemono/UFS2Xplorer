add_library(ps3hdd_fs STATIC
    ufs2_types.cpp
    ufs2_types.h
    ufs2_filesystem.cpp
    ufs2_filesystem.h
    ufs2_writer.cpp
    ufs2_writer.h
    ffs_policy.cpp
    ffs_policy.h
    ufs2_checker.cpp
    ufs2_checker.h
)

target_include_directories(ps3hdd_fs PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)

target_link_libraries(ps3hdd_fs PUBLIC ps3hdd_disk)