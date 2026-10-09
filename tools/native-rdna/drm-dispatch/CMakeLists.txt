cmake_minimum_required(VERSION 3.20)
project(DrmDispatch CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR "drm_dispatch submits through the Linux amdgpu kernel interface")
endif()

find_program(LLVM_MC NAMES llvm-mc llvm-mc-20 llvm-mc-19 llvm-mc-18 REQUIRED)
find_program(LLVM_OBJCOPY NAMES llvm-objcopy llvm-objcopy-20 llvm-objcopy-19 llvm-objcopy-18 REQUIRED)

set(kernelSource ${CMAKE_CURRENT_SOURCE_DIR}/kernel.s)
set(kernelObject ${CMAKE_CURRENT_BINARY_DIR}/kernel.o)
set(kernelBinary ${CMAKE_CURRENT_BINARY_DIR}/kernel.bin)
set(kernelHeader ${CMAKE_CURRENT_BINARY_DIR}/KernelBinary.hpp)
add_custom_command(
    OUTPUT ${kernelHeader}
    COMMAND ${LLVM_MC} -triple=amdgcn-amd-amdhsa -mcpu=gfx1201 -filetype=obj ${kernelSource} -o ${kernelObject}
    COMMAND ${LLVM_OBJCOPY} -O binary --only-section=.text ${kernelObject} ${kernelBinary}
    COMMAND ${CMAKE_COMMAND} -DINPUT=${kernelBinary} -DOUTPUT=${kernelHeader} -P ${CMAKE_CURRENT_SOURCE_DIR}/EmbedKernel.cmake
    DEPENDS ${kernelSource} ${CMAKE_CURRENT_SOURCE_DIR}/EmbedKernel.cmake
    VERBATIM
)
add_custom_target(drm_dispatch_kernel DEPENDS ${kernelHeader})

add_executable(drm_dispatch DrmDispatch.cpp)
target_include_directories(drm_dispatch PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_BINARY_DIR})
add_dependencies(drm_dispatch drm_dispatch_kernel)

enable_testing()
add_executable(drm_dispatch_pm4_tests Pm4Tests.cpp)
target_include_directories(drm_dispatch_pm4_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_BINARY_DIR})
add_dependencies(drm_dispatch_pm4_tests drm_dispatch_kernel)
add_test(NAME drm_dispatch_pm4 COMMAND drm_dispatch_pm4_tests)
