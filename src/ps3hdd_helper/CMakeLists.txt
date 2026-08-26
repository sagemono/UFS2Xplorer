add_executable(ps3hdd_helper main.cpp)

set_target_properties(ps3hdd_helper PROPERTIES
    WIN32_EXECUTABLE TRUE
)

target_link_libraries(ps3hdd_helper PRIVATE
    ps3hdd_ipc
    ps3hdd_disk
    Qt6::Core
    Qt6::Network
)