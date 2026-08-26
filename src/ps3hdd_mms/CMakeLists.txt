add_library(ps3hdd_mms STATIC metadata_db.cpp metadata_db.h idx_install.cpp idx_install.h)
target_include_directories(ps3hdd_mms PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(ps3hdd_mms PUBLIC ps3hdd_crypto)