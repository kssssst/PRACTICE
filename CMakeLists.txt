cmake_minimum_required(VERSION 3.20)
project(TrayApp)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO "${CMAKE_BINARY_DIR}")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_MINSIZEREL "${CMAKE_BINARY_DIR}")

# Статическая линковка CRT
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# Поддержка UTF-8 для MSVC
add_compile_options("$<$<C_COMPILER_ID:MSVC>:/utf-8>")
add_compile_options("$<$<CXX_COMPILER_ID:MSVC>:/utf-8>")

# Поддержка компилятора ресурсов
enable_language(RC)
set(CMAKE_RC_FLAGS "${CMAKE_RC_FLAGS} /l 0x409 /r")

# Генерация RPC клиентского кода
set(RPC_OUTPUT_DIR "${CMAKE_BINARY_DIR}/rpc")
file(MAKE_DIRECTORY "${RPC_OUTPUT_DIR}")

add_custom_command(
    OUTPUT "${RPC_OUTPUT_DIR}/TrayService_c.c" "${RPC_OUTPUT_DIR}/TrayService_s.c" "${RPC_OUTPUT_DIR}/TrayService.h"
    COMMAND midl.exe /acf "${CMAKE_CURRENT_SOURCE_DIR}/service/src/TrayService.acf"
            /cstub "${RPC_OUTPUT_DIR}/TrayService_c.c"
            /sstub "${RPC_OUTPUT_DIR}/TrayService_s.c"
            /header "${RPC_OUTPUT_DIR}/TrayService.h"
            "${CMAKE_CURRENT_SOURCE_DIR}/service/src/TrayService.idl"
    DEPENDS
            "${CMAKE_CURRENT_SOURCE_DIR}/service/src/TrayService.idl"
            "${CMAKE_CURRENT_SOURCE_DIR}/service/src/TrayService.acf"
    COMMENT "Generating RPC client files from IDL"
)

# Исполняемый файл графического приложения
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

# Добавляем проект службы как поддиректорию
add_subdirectory(service)
