cmake_minimum_required(VERSION 3.24)
project(tabdisplay_host LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

add_executable(tabdisplay-host
    src/main.cpp
    src/capture.cpp
    src/nvenc.cpp
    src/net.cpp
    src/adb.cpp
    src/inject.cpp
    src/stats.cpp
)

target_include_directories(tabdisplay-host PRIVATE third_party/nv-codec-headers)
target_compile_definitions(tabdisplay-host PRIVATE
    UNICODE
    _UNICODE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    _WIN32_WINNT=0x0A00
    NTDDI_VERSION=0x0A000006
)
target_compile_options(tabdisplay-host PRIVATE /W4 /permissive- /utf-8 /EHsc)
target_link_libraries(tabdisplay-host PRIVATE d3d11 dxgi ws2_32 winmm user32)
