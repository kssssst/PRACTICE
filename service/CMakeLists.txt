cmake_minimum_required(VERSION 3.20)
project(TrayService)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO "${CMAKE_BINARY_DIR}")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_MINSIZEREL "${CMAKE_BINARY_DIR}")

set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

add_compile_options("$<$<C_COMPILER_ID:MSVC>:/utf-8>")
add_compile_options("$<$<CXX_COMPILER_ID:MSVC>:/utf-8>")

set(RPC_OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/rpc")
file(MAKE_DIRECTORY "${RPC_OUTPUT_DIR}")

add_custom_command(
    OUTPUT "${RPC_OUTPUT_DIR}/TrayService_s.c" "${RPC_OUTPUT_DIR}/TrayService.h"
    COMMAND midl.exe /acf "${CMAKE_CURRENT_SOURCE_DIR}/src/TrayService.acf"
            /server stub /out "${RPC_OUTPUT_DIR}"
            "${CMAKE_CURRENT_SOURCE_DIR}/src/TrayService.idl"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/TrayService.idl"
            "${CMAKE_CURRENT_SOURCE_DIR}/src/TrayService.acf"
    COMMENT "Generating RPC server files"
)

add_executable(TrayService
    src/TrayService.cpp
    "${RPC_OUTPUT_DIR}/TrayService_s.c"
)

target_compile_definitions(TrayService PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_include_directories(TrayService PRIVATE "${RPC_OUTPUT_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(TrayService PRIVATE rpcrt4 wtsapi32 userenv advapi32 shell32 user32 gdi32 winhttp iphlpapi)

set_target_properties(TrayService PROPERTIES VS_EXECUTABLE_SUBSYSTEM CONSOLE)
