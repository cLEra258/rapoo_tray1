cmake_minimum_required(VERSION 3.20)
project(RapooTray LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

find_package(hidapi CONFIG REQUIRED)

# 保底：确保 hidapi 头文件目录被加入 include 搜索路径
find_path(HIDAPI_INCLUDE_DIR NAMES hidapi/hidapi.h)
if(NOT HIDAPI_INCLUDE_DIR)
    message(FATAL_ERROR "找不到 hidapi/hidapi.h，请检查 vcpkg 是否安装了 hidapi")
endif()

add_executable(rapoo_tray WIN32 rapoo_tray.cpp)
target_include_directories(rapoo_tray PRIVATE ${HIDAPI_INCLUDE_DIR})
target_link_libraries(rapoo_tray PRIVATE hidapi::hidapi user32 gdi32 shell32)

if (MSVC)
    target_compile_options(rapoo_tray PRIVATE /O2 /GL /Gy /Gw)
    target_link_options(rapoo_tray PRIVATE /LTCG /OPT:REF /OPT:ICF)
endif()
