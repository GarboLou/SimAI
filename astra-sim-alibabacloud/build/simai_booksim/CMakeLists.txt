cmake_minimum_required(VERSION 3.15)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED True)

if (CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    if (CMAKE_CXX_COMPILER_VERSION VERSION_LESS 5.3)
        message(FATAL_ERROR "g++ (GNU) version should be greater than 5.3, but found ${CMAKE_CXX_COMPILER_VERSION}")
    endif()
endif()

project (AstraSim_BookSim)

# SimCCL mock version selection (matches the ns3 and analytical builds)
if(DEFINED ENV{SIMAI_NCCL_VERSION})
    set(MOCK_VERSION $ENV{SIMAI_NCCL_VERSION})
else()
    set(MOCK_VERSION "v2.30")
endif()

include_directories("${PROJECT_SOURCE_DIR}/../../")
include_directories("${PROJECT_SOURCE_DIR}/../../../")
# Sys.hh does #include "SimCCL/mock/MockNcclChannel.h"; this symlink is what
# makes that resolve to the selected mock version. Do not drop it.
file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/SimCCL")
file(CREATE_LINK "${PROJECT_SOURCE_DIR}/../../../SimCCL/src/mock/${MOCK_VERSION}" "${CMAKE_BINARY_DIR}/SimCCL/mock" SYMBOLIC)
include_directories("${CMAKE_BINARY_DIR}")

add_subdirectory("${PROJECT_SOURCE_DIR}/../../" AstraSim)
add_subdirectory ("${PROJECT_SOURCE_DIR}/../../astra-sim/network_frontend/booksim" simai_booksim)
