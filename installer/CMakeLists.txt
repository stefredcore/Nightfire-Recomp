cmake_minimum_required(VERSION 3.20)

# Nightfire-Setup.exe, the installer for players. A project of its own: the
# game's CMakeLists.txt needs the generated code, this needs none. Built by
# installer/build.ps1, which first packs the payload into PAYLOAD_DIR.
project(nightfire_setup C RC)

set(PAYLOAD_DIR "${CMAKE_CURRENT_SOURCE_DIR}/obj" CACHE PATH "Folder with project.zip, toolkit.zip, manifest.txt")
get_filename_component(NF_ICON "${CMAKE_CURRENT_SOURCE_DIR}/../assets/nightfire.ico" ABSOLUTE)
configure_file(setup.rc.in "${CMAKE_CURRENT_BINARY_DIR}/setup.rc" @ONLY)
set_source_files_properties("${CMAKE_CURRENT_BINARY_DIR}/setup.rc" PROPERTIES OBJECT_DEPENDS
    "${PAYLOAD_DIR}/project.zip;${PAYLOAD_DIR}/toolkit.zip;${PAYLOAD_DIR}/manifest.txt;${PAYLOAD_DIR}/toolchain.zip")

add_executable(nightfire_setup WIN32 nightfire_setup.c "${CMAKE_CURRENT_BINARY_DIR}/setup.rc")
set_target_properties(nightfire_setup PROPERTIES OUTPUT_NAME "Nightfire-Setup")
target_link_libraries(nightfire_setup PRIVATE comctl32 comdlg32 shell32 shlwapi ole32 uuid msimg32 advapi32)
if(MSVC)
    # The C runtime linked in: a player's PC need not have Visual C++'s.
    set_property(TARGET nightfire_setup PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded")
    target_compile_options(nightfire_setup PRIVATE /utf-8 /W4)
    target_link_options(nightfire_setup PRIVATE /ENTRY:wWinMainCRTStartup)
endif()
