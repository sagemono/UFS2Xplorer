add_library(ps3hdd_app STATIC gameos.cpp gameos.h database.cpp database.h)
target_include_directories(ps3hdd_app PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(ps3hdd_app PUBLIC ps3hdd_fs ps3hdd_cryptodisk ps3hdd_crypto ps3hdd_disk)