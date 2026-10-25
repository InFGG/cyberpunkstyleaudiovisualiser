cmake_minimum_required(VERSION 3.25)
project(CyberpsychosisOverlay LANGUAGES CXX)

add_executable(CyberpsychosisOverlay WIN32 main.cpp)
target_compile_features(CyberpsychosisOverlay PRIVATE cxx_std_17)
target_compile_definitions(CyberpsychosisOverlay PRIVATE UNICODE _UNICODE)
target_link_libraries(CyberpsychosisOverlay PRIVATE
    d3d11
    dxgi
    dcomp
    d3dcompiler
    ole32
    uuid
)

configure_file(glitch.hlsl glitch.hlsl COPYONLY)
