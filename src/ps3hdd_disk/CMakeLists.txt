add_library(ps3hdd_disk STATIC
    disk_source.cpp
    disk_source.h
    aligned_buffer.h
    image_disk_source.cpp
    image_disk_source.h
    raw_device.h
    physical_disk_source.cpp
    physical_disk_source.h
)

if(WIN32)
    target_sources(ps3hdd_disk PRIVATE raw_device_windows.cpp)
    target_link_libraries(ps3hdd_disk PUBLIC setupapi cfgmgr32)
elseif(UNIX)
    target_sources(ps3hdd_disk PRIVATE raw_device_posix.cpp)
endif()

target_include_directories(ps3hdd_disk PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)