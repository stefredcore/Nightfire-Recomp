cmake_minimum_required(VERSION 3.20)

# ============================================================================
# 007: Nightfire - static recompilation
#
# Two executables from the two XBEs on the disc:
#   nightfire          default.xbe  (src/recomp/main/gen)
#   nightfire_driving  Driving.xbe  (src/recomp/driving/gen)
# The generated code is not part of the repository: scripts/recompile.ps1
# produces it from your own game files.
#
# Visual Studio builds it for development; a player's install builds it with
# Zig's Clang (cmake/zig-windows.cmake, from toolchain\, see scripts/build.ps1).
# ============================================================================

project(nightfire C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

add_compile_definitions(WIN32_LEAN_AND_MEAN NOMINMAX)

# ── Toolkit ─────────────────────────────────────────────────────
# The submodule by default; -DXBOXRECOMP_DIR=<path> builds against another
# checkout of the nightfire branch.
set(XBOXRECOMP_DIR "${CMAKE_CURRENT_SOURCE_DIR}/external/xboxrecomp" CACHE PATH
    "Path to the xboxrecomp toolkit root directory")
if(NOT EXISTS "${XBOXRECOMP_DIR}/CMakeLists.txt")
    message(FATAL_ERROR "xboxrecomp not found at ${XBOXRECOMP_DIR}. "
                        "Run: git submodule update --init --recursive")
endif()
add_subdirectory(${XBOXRECOMP_DIR} ${CMAKE_BINARY_DIR}/xboxrecomp)

# The toolkit's own test programs are not part of the game: built only when
# asked for by name, not beside nightfire.exe in every build.
foreach(t d3d8_gamma d3d8_smoke d3d8_states_smoke d3d8_a8)
    if(TARGET ${t})
        set_target_properties(${t} PROPERTIES EXCLUDE_FROM_ALL TRUE)
    endif()
endforeach()

# Debug info for the runtime too, so a profile or a crash names its static
# functions (the rasteriser, the combiners) instead of the nearest export.
if(MSVC)
    target_compile_options(xbox_kernel PRIVATE /Zi)
endif()

# ── The icon ────────────────────────────────────────────────────
# assets/nightfire.ico as resource 1 of every program (the window classes load
# it from there). Visual Studio compiles a .rc written here with the icon's
# full path (its rc looks for a relative one in the build folder). zig cc
# compiles a .rc handed to the link and failed on that copy
# (FileSystemFailure); it takes assets/nightfire.rc, which finds the icon
# beside it, as it does launcher/launcher.rc.
file(TO_CMAKE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/assets/nightfire.ico" NF_ICON)
# The same resources as assets/nightfire.rc (icon and version information),
# with the icon's full path.
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/assets/nightfire.rc" NF_RC)
string(REPLACE "\"nightfire.ico\"" "\"${NF_ICON}\"" NF_RC "${NF_RC}")
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/nightfire_icon.rc" "${NF_RC}")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/assets/nightfire.rc")
function(nightfire_add_icon name)
    if(MSVC)
        target_sources(${name} PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/nightfire_icon.rc")
    else()
        set(rc "${CMAKE_CURRENT_SOURCE_DIR}/assets/nightfire.rc")
        target_link_options(${name} PRIVATE "${rc}")
        set_property(TARGET ${name} APPEND PROPERTY LINK_DEPENDS "${rc}" "${NF_ICON}")
    endif()
endfunction()

# ── Executables ─────────────────────────────────────────────────
# nightfire_add_exe(<exe name> <target: main|driving>)
function(nightfire_add_exe name target)
    set(gen "${CMAKE_CURRENT_SOURCE_DIR}/src/recomp/${target}/gen")
    if(NOT EXISTS "${gen}/recomp_funcs.h")
        message(FATAL_ERROR "No generated code in ${gen}. "
                            "Run scripts/recompile.ps1 first.")
    endif()
    # CONFIGURE_DEPENDS: a new lift can add or drop chunk files.
    file(GLOB gen_sources CONFIGURE_DEPENDS "${gen}/*.c")

    add_executable(${name} WIN32
        src/main.c
        src/recomp_manual.c
        src/manual_${target}.c
        src/nv2a_d3d11_backend.c
        ${gen_sources})
    target_include_directories(${name} PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${gen}
        ${XBOXRECOMP_DIR}/include
        ${XBOXRECOMP_DIR}/src/kernel         # nv2a_backend.h
        ${XBOXRECOMP_DIR}/src/d3d)           # d3d8_swizzle.h
    target_link_libraries(${name} PRIVATE
        xboxrecomp d3d11 dxgi dxguid
        $<IF:$<BOOL:${MINGW}>,d3dcompiler_47,d3dcompiler>
        $<IF:$<BOOL:${MINGW}>,xinput1_4,xinput>
        winmm
        shell32                             # the saves' folder in Documents
        dbghelp)                            # crash handler names sub_XXXXXXXX
    if(MSVC)
        # /bigobj for the large chunks; a PDB in Release too, which the crash
        # handler and the profiler resolve guest functions through.
        target_compile_options(${name} PRIVATE /bigobj /Zi /MP)
        target_link_options(${name} PRIVATE /LARGEADDRESSAWARE /DEBUG /OPT:REF /OPT:ICF)
    else()
        # Zig's Clang (cmake/zig-windows.cmake), which writes the PDB by
        # itself. The lifted code's `if ((x == 0))` warns thousands of times.
        target_compile_options(${name} PRIVATE -Wno-parentheses-equality)
        # A window program, no console: zig cc ignores the -mwindows that
        # WIN32 adds, the linker takes the subsystem directly.
        target_link_options(${name} PRIVATE -Wl,--subsystem,windows)
    endif()
    nightfire_add_icon(${name})
endfunction()

# ON: the toolkit and the launcher only, which need no generated code, so
# scripts/build.ps1 -Prepare can compile them while the lift still runs.
option(NIGHTFIRE_TOOLKIT_ONLY "Leave out the two game executables" OFF)
if(NOT NIGHTFIRE_TOOLKIT_ONLY)
    nightfire_add_exe(nightfire main)
    nightfire_add_exe(nightfire_driving driving)
    target_compile_definitions(nightfire_driving PRIVATE NIGHTFIRE_DRIVING=1)
endif()

# Mode, start, settings and updates, see the head of the file.
add_executable(nightfire_launcher WIN32 launcher/nightfire_launcher.c)
target_link_libraries(nightfire_launcher PRIVATE comctl32 shell32 msimg32)
nightfire_add_icon(nightfire_launcher)
if(MSVC)
    target_compile_options(nightfire_launcher PRIVATE /utf-8)
    target_link_options(nightfire_launcher PRIVATE /ENTRY:wWinMainCRTStartup)
else()
    target_link_options(nightfire_launcher PRIVATE -municode    # wWinMain
                                                   -Wl,--subsystem,windows)
    # The manifest for Common Controls 6, which MSVC takes from the source's
    # #pragma comment(linker): zig cc compiles the .rc and links it in.
    target_link_options(nightfire_launcher PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/launcher/launcher.rc")
    set_property(TARGET nightfire_launcher APPEND PROPERTY LINK_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/launcher/launcher.rc"
        "${CMAKE_CURRENT_SOURCE_DIR}/launcher/launcher.manifest")
endif()
