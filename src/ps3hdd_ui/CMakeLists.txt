qt_add_executable(ps3hdd_ui
    main.cpp
    main_window.cpp
    main_window.h
    ufs2_model.cpp
    ufs2_model.h
    worker.cpp
    worker.h
    license_dialog.cpp
    license_dialog.h
    console_wizard.cpp
    console_wizard.h
    drives_dialog.cpp
    drives_dialog.h
    profile_dialog.cpp
    profile_dialog.h
    game_preview.cpp
    game_preview.h
    game_size_scanner.cpp
    game_size_scanner.h
    fs_util.cpp
    fs_util.h
    install_dialog.cpp
    install_dialog.h
    settings_dialog.cpp
    settings_dialog.h
    sfo_editor.cpp
    sfo_editor.h
    sfo_util.cpp
    sfo_util.h
    reveal_field.h
    key_store.cpp
    key_store.h
    icons.qrc
)

set_target_properties(ps3hdd_ui PROPERTIES
    OUTPUT_NAME UFS2Xplorer
    AUTOMOC ON
    AUTORCC ON
    WIN32_EXECUTABLE TRUE
)

target_include_directories(ps3hdd_ui PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)

target_link_libraries(ps3hdd_ui PRIVATE
    ps3hdd_app
    ps3hdd_pkg
    ps3hdd_license
    ps3hdd_mms
    ps3hdd_fs
    ps3hdd_disk
    ps3hdd_ipc
    Qt6::Core
    Qt6::Widgets
    Qt6::Network
)

if(WIN32)
    get_target_property(_qt_moc Qt6::moc IMPORTED_LOCATION)
    get_filename_component(_qt_bin "${_qt_moc}" DIRECTORY)
    find_program(WINDEPLOYQT NAMES windeployqt windeployqt.exe HINTS "${_qt_bin}")
    if(WINDEPLOYQT)
        add_custom_command(TARGET ps3hdd_ui POST_BUILD
            COMMAND "${WINDEPLOYQT}" --no-translations --no-compiler-runtime "$<TARGET_FILE:ps3hdd_ui>"
            COMMENT "Deploying Qt runtime next to ps3hdd_ui"
            VERBATIM)
    else()
        message(WARNING "windeployqt not found; ps3hdd_ui may not launch from the build tree.")
    endif()
endif()