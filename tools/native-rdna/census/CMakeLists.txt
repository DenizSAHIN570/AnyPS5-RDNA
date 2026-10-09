cmake_minimum_required(VERSION 3.20)
project(ShaderCensus CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

get_filename_component(ANYPS5_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../.." ABSOLUTE)
set(RECOMPILER_ROOT "${ANYPS5_ROOT}/core/shader/recompiler")

file(GLOB recompilerIncludeDirectories LIST_DIRECTORIES true "${RECOMPILER_ROOT}/*/include")
file(GLOB rdnaDecoderSources CONFIGURE_DEPENDS "${RECOMPILER_ROOT}/RdnaDecoder/src/*.cpp")

add_library(census_rdna_decoder STATIC ${rdnaDecoderSources})
target_include_directories(census_rdna_decoder PUBLIC ${RECOMPILER_ROOT} ${recompilerIncludeDirectories})

add_library(census_core STATIC Census.cpp)
target_include_directories(census_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR} "${ANYPS5_ROOT}/core/libs")
target_link_libraries(census_core PUBLIC census_rdna_decoder)

add_executable(shader_census main.cpp)
target_link_libraries(shader_census PRIVATE census_core)

enable_testing()
add_executable(shader_census_tests CensusTests.cpp)
target_link_libraries(shader_census_tests PRIVATE census_core)
add_test(NAME shader_census COMMAND shader_census_tests)
