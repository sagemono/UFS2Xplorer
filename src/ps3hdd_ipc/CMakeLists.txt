add_library(ps3hdd_ipc STATIC
    ipc_protocol.cpp
    ipc_protocol.h
    ipc_client.cpp
    ipc_client.h
    ipc_server.cpp
    ipc_server.h
)

set_target_properties(ps3hdd_ipc PROPERTIES AUTOMOC ON)

target_include_directories(ps3hdd_ipc PUBLIC ${CMAKE_SOURCE_DIR}/src)

target_link_libraries(ps3hdd_ipc PUBLIC
    ps3hdd_disk
    Qt6::Core
    Qt6::Network
)