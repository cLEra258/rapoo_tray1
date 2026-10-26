cmake_minimum_required(VERSION 3.20)
project(RapooTray LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

find_package(hidapi CONFIG REQUIRED)

add_executable(rapoo_tray WIN32 rapoo_tray.cpp)
target_link_libraries(rapoo_tray PRIVATE hidapi::hidapi user32 gdi32 shell32)

if (MSVC)
    target_compile_options(rapoo_tray PRIVATE /O2 /GL /Gy /Gw)
    target_link_options(rapoo_tray PRIVATE /LTCG /OPT:REF /OPT:ICF)
endif()