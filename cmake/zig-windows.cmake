# Building with Zig's Clang for x86_64-windows-gnu (MinGW-w64 headers and
# runtime) instead of Visual Studio: what a player's install builds with, from
# the tools in toolchain\ (scripts/build.ps1 passes this file and ZIG_EXE).
#
#     cmake -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/zig-windows.cmake -DZIG_EXE=...
#
# CMake wants each tool as a single program, so this writes a one-line .cmd
# per tool (zig cc, zig ar, zig ranlib) into the build folder.

set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

if(NOT ZIG_EXE)
    set(ZIG_EXE "$ENV{ZIG_EXE}")
endif()
if(NOT EXISTS "${ZIG_EXE}")
    message(FATAL_ERROR "zig-windows.cmake: ZIG_EXE does not name zig.exe (got '${ZIG_EXE}')")
endif()
# Passed on to try_compile's own configure runs.
list(APPEND CMAKE_TRY_COMPILE_PLATFORM_VARIABLES ZIG_EXE)

file(TO_NATIVE_PATH "${ZIG_EXE}" _zig)
set(_wrap "${CMAKE_BINARY_DIR}/zig-wrap")
file(MAKE_DIRECTORY "${_wrap}")
file(WRITE "${_wrap}/zig-cc.cmd"     "@\"${_zig}\" cc -target x86_64-windows-gnu %*\r\n")
file(WRITE "${_wrap}/zig-ar.cmd"     "@\"${_zig}\" ar %*\r\n")
file(WRITE "${_wrap}/zig-ranlib.cmd" "@\"${_zig}\" ranlib %*\r\n")

set(CMAKE_C_COMPILER "${_wrap}/zig-cc.cmd")
set(CMAKE_C_COMPILER_TARGET x86_64-windows-gnu)
set(CMAKE_AR     "${_wrap}/zig-ar.cmd"     CACHE FILEPATH "" FORCE)
set(CMAKE_RANLIB "${_wrap}/zig-ranlib.cmd" CACHE FILEPATH "" FORCE)
