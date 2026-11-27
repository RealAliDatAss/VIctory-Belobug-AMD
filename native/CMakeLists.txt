cmake_minimum_required(VERSION 3.20)
if(POLICY CMP0141)
    cmake_policy(SET CMP0141 NEW)
endif()
set(CMAKE_MSVC_DEBUG_INFORMATION_FORMAT "Embedded" CACHE STRING "" FORCE)
project(victory_native LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(victory_native_core STATIC
    src/mem_guard.cpp
    src/xmem_core.cpp
    src/hook_x64.cpp
    src/d3d12_probe.cpp
    src/d3d12_host.cpp
)

target_include_directories(victory_native_core
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_compile_definitions(victory_native_core
    PUBLIC
        WIN32_LEAN_AND_MEAN
        NOMINMAX
        _CRT_SECURE_NO_WARNINGS
)

if(MSVC)
    target_compile_options(victory_native_core PRIVATE /W4 /EHsc /permissive-)
endif()

target_link_libraries(victory_native_core
    PUBLIC
        d3d12
        dxgi
        dxguid
        d3dcompiler
        onecore
)

add_library(victory_native SHARED
    src/victory_abi.cpp
)

target_compile_definitions(victory_native PRIVATE VICTORY_NATIVE_EXPORTS=1)
if(MSVC)
    target_compile_options(victory_native PRIVATE /W4 /EHsc /permissive-)
endif()
target_link_libraries(victory_native PRIVATE victory_native_core)

add_executable(victory_probe
    src/probe_main.cpp
)

if(MSVC)
    target_compile_options(victory_probe PRIVATE /W4 /EHsc /permissive-)
endif()
target_link_libraries(victory_probe PRIVATE victory_native victory_native_core)
