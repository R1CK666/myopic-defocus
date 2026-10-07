cmake_minimum_required(VERSION 3.20)
project(MyopicDefocus LANGUAGES CXX RC)

add_executable(MyopicDefocus WIN32
    src/main.cpp
    resources/app.rc
)

target_compile_features(MyopicDefocus PRIVATE cxx_std_20)
target_compile_definitions(MyopicDefocus PRIVATE
    UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX
)
target_compile_options(MyopicDefocus PRIVATE
    -O3 -Wall -Wextra -Wpedantic -ffunction-sections -fdata-sections
)
target_link_options(MyopicDefocus PRIVATE
    -municode -static -static-libgcc -static-libstdc++ -Wl,--gc-sections
)
target_link_libraries(MyopicDefocus PRIVATE
    d3d11 dxgi d3dcompiler dwmapi shell32 shlwapi ole32 user32 gdi32
)
