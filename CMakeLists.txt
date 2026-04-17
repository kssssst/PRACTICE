cmake_minimum_required(VERSION 3.20)
project(TrayApp)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Включаем Resource Compiler для RC файлов
enable_language(RC)

# Основной исполняемый файл
add_executable(TrayApp WIN32 
    resources/src/tray_win32.cpp
    TrayApp.rc
)

target_compile_definitions(TrayApp PRIVATE 
    UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX
)

target_link_libraries(TrayApp PRIVATE
    shell32 user32 gdi32 comctl32
)

# Установка выходной директории
set_target_properties(TrayApp PROPERTIES
    VS_EXECUTABLE_SUBSYSTEM WIN32
)