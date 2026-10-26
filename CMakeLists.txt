cmake_minimum_required(VERSION 3.20)
project(RapooTray LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

find_package(hidapi CONFIG REQUIRED)

find_path(HIDAPI_INCLUDE_DIR NAMES hidapi/hidapi.h)
if(NOT HIDAPI_INCLUDE_DIR)
    message(FATAL_ERROR "找不到 hidapi/hidapi.h")
endif()

add_executable(rapoo_tray WIN32 rapoo_tray.cpp)
target_include_directories(rapoo_tray PRIVATE ${HIDAPI_INCLUDE_DIR})

# 加入了 gdiplus
target_link_libraries(rapoo_tray PRIVATE hidapi::hidapi user32 gdi32 shell32 gdiplus)

if (MSVC)
    target_compile_options(rapoo_tray PRIVATE /utf-8 /O2 /GL /Gy /Gw)
    target_link_options(rapoo_tray PRIVATE /LTCG /OPT:REF /OPT:ICF)
endif()
