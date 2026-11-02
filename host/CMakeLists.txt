cmake_minimum_required(VERSION 3.24)
project(tabdisplay_host LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

add_library(tabdisplay_core STATIC
    src/engine.cpp
    src/display.cpp
    src/capture.cpp
    src/nvenc.cpp
    src/mfenc.cpp
    src/net.cpp
    src/adb.cpp
    src/inject.cpp
    src/stats.cpp
)

target_include_directories(tabdisplay_core PUBLIC src third_party/nv-codec-headers)
target_compile_definitions(tabdisplay_core PUBLIC
    UNICODE
    _UNICODE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    _WIN32_WINNT=0x0A00
    NTDDI_VERSION=0x0A000006
)
target_compile_options(tabdisplay_core PUBLIC /W4 /permissive- /utf-8 /EHsc)
target_link_libraries(tabdisplay_core PUBLIC d3d11 dxgi ws2_32 winmm user32 ole32 oleaut32 mfplat mfuuid strmiids)

add_executable(tabdisplay-host src/main.cpp)
target_link_libraries(tabdisplay-host PRIVATE tabdisplay_core)

add_executable(TabDisplay WIN32 src/tray.cpp)
target_link_libraries(TabDisplay PRIVATE tabdisplay_core d2d1 dwrite dwmapi windowscodecs shell32 advapi32 ole32)
