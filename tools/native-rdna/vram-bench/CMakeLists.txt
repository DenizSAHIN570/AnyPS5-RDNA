cmake_minimum_required(VERSION 3.20)
project(VramBench CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR "vram_bench measures Linux-only paths (placed maps over the DRM driver, udmabuf)")
endif()

get_filename_component(ANYPS5_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../.." ABSOLUTE)
set(VRAM_BENCH_VULKAN_HEADERS "${ANYPS5_ROOT}/3rdparty/Vulkan-Headers/include" CACHE PATH "Vulkan-Headers include directory with VK_EXT_map_memory_placed")
if(NOT EXISTS "${VRAM_BENCH_VULKAN_HEADERS}/vulkan/vulkan_core.h")
    message(FATAL_ERROR "Vulkan headers not found at ${VRAM_BENCH_VULKAN_HEADERS}: run git submodule update --init 3rdparty/Vulkan-Headers or set VRAM_BENCH_VULKAN_HEADERS")
endif()

add_executable(vram_bench VramBench.cpp)
target_include_directories(vram_bench PRIVATE ${VRAM_BENCH_VULKAN_HEADERS})
target_link_libraries(vram_bench PRIVATE ${CMAKE_DL_LIBS})
