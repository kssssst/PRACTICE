cmake_minimum_required(VERSION 3.20)
project(TrayApp)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Статическое связывание Visual C++ Runtime
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# Поддержка UTF-8 кодировки для MSVC компилятора
add_compile_options("$<$<C_COMPILER_ID:MSVC>:/utf-8>")
add_compile_options("$<$<CXX_COMPILER_ID:MSVC>:/utf-8>")

# Включаем Resource Compiler для RC файлов
enable_language(RC)
set(CMAKE_RC_FLAGS "${CMAKE_RC_FLAGS} /l 0x409 /r")

# Основной исполняемый файл
add_executable(TrayApp WIN32 
    resources/src/tray_win32.cpp
    TrayApp.rc
)

# Определения для компилятора C++
target_compile_definitions(TrayApp PRIVATE 
    UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX
)

# Включаемые директории
target_include_directories(TrayApp PRIVATE
    ${CMAKE_SOURCE_DIR}/resources/src
)

# Связываемые библиотеки
target_link_libraries(TrayApp PRIVATE
    shell32 user32 gdi32 comctl32
)

# Свойства целевого приложения
set_target_properties(TrayApp PROPERTIES
    VS_EXECUTABLE_SUBSYSTEM WIN32
    WIN32_EXECUTABLE ON
)