cmake_minimum_required(VERSION 3.20)
project(TrayApp)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Все исполняемые файлы будут в корне build/
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO "${CMAKE_BINARY_DIR}")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_MINSIZEREL "${CMAKE_BINARY_DIR}")

# Статическая линковка CRT
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# UTF-8 поддержка
add_compile_options("$<$<C_COMPILER_ID:MSVC>:/utf-8>")
add_compile_options("$<$<CXX_COMPILER_ID:MSVC>:/utf-8>")

# Компилятор ресурсов
enable_language(RC)
set(CMAKE_RC_FLAGS "${CMAKE_RC_FLAGS} /l 0x409 /r")

# ====================== RPC ======================
set(RPC_OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/rpc")
file(MAKE_DIRECTORY "${RPC_OUTPUT_DIR}")

# Генерация RPC клиентских файлов
add_custom_command(
    OUTPUT "${RPC_OUTPUT_DIR}/TrayService_c.c" "${RPC_OUTPUT_DIR}/TrayService.h"
    COMMAND midl.exe /acf "${CMAKE_CURRENT_SOURCE_DIR}/service/src/TrayService.acf"
            /client stub /out "${RPC_OUTPUT_DIR}"
            "${CMAKE_CURRENT_SOURCE_DIR}/service/src/TrayService.idl"
    DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/service/src/TrayService.idl"
        "${CMAKE_CURRENT_SOURCE_DIR}/service/src/TrayService.acf"
    COMMENT "Generating RPC client files from IDL"
)

# ====================== TrayApp ======================
add_executable(TrayApp WIN32
    resources/src/tray_win32.cpp
    resources/src/TrayServiceClient.cpp
    "${RPC_OUTPUT_DIR}/TrayService_c.c"
    TrayApp.rc
)

target_compile_definitions(TrayApp PRIVATE
    UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX
)

target_include_directories(TrayApp PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/resources/src
    "${RPC_OUTPUT_DIR}"
)

target_link_libraries(TrayApp PRIVATE
    shell32 user32 gdi32 comctl32 rpcrt4 advapi32 psapi
)

set_target_properties(TrayApp PROPERTIES
    VS_EXECUTABLE_SUBSYSTEM WIN32
    WIN32_EXECUTABLE ON
)

# ====================== TrayService ======================
add_subdirectory(service)

# ====================== Копирование для удобства ======================
# Эти команды теперь используют BYPRODUCTS + DEPENDS, чтобы избежать ошибки "TARGET not created in this directory"
add_custom_command(TARGET TrayApp POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        $<TARGET_FILE:TrayApp>
        "${CMAKE_BINARY_DIR}/TrayApp.exe"
    COMMENT "Copying TrayApp.exe to build root"
)

add_custom_command(TARGET TrayService POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        $<TARGET_FILE:TrayService>
        "${CMAKE_BINARY_DIR}/TrayService.exe"
    COMMENT "Copying TrayService.exe to build root"
)

message(STATUS "CMake configuration completed successfully for ARM64.")