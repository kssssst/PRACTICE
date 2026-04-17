cmake_minimum_required(VERSION 3.20)
project(TrayService)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Static linking for Visual C++ Runtime
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# UTF-8 support for MSVC
add_compile_options("$<$<C_COMPILER_ID:MSVC>:/utf-8>")
add_compile_options("$<$<CXX_COMPILER_ID:MSVC>:/utf-8>")

# RPC output directory
set(RPC_OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/rpc")
file(MAKE_DIRECTORY "${RPC_OUTPUT_DIR}")

# Generate RPC server files from IDL
add_custom_command(
    OUTPUT "${RPC_OUTPUT_DIR}/TrayService_s.c" "${RPC_OUTPUT_DIR}/TrayService.h"
    COMMAND midl.exe /server stub /out "${RPC_OUTPUT_DIR}" 
            "${CMAKE_CURRENT_SOURCE_DIR}/src/TrayService.idl"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/TrayService.idl"
    COMMENT "Generating RPC server files from IDL"
)

# Service executable
add_executable(TrayService
    src/TrayService.cpp
    "${RPC_OUTPUT_DIR}/TrayService_s.c"
)

target_compile_definitions(TrayService PRIVATE 
    UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX
)

target_include_directories(TrayService PRIVATE
    "${RPC_OUTPUT_DIR}"
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
)

target_link_libraries(TrayService PRIVATE
    rpcrt4 wtsapi32 userenv advapi32 shell32 user32 gdi32
)

set_target_properties(TrayService PROPERTIES
    VS_EXECUTABLE_SUBSYSTEM CONSOLE
)
