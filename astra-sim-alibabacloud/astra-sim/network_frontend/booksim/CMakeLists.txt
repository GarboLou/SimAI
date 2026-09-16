cmake_minimum_required(VERSION 3.15)
project(SimAI_booksim)

file(GLOB SOURCES "*.cc")
file(GLOB HEADERS "*.h")
include_directories("${PROJECT_SOURCE_DIR}/../../../")

# BookSim is built from its own tree as a static library.
# Probe the usual layouts rather than assuming one: the checkout is sometimes
# booksim2/src and sometimes booksim2/booksim2/src depending on how it was
# cloned. An explicit -DBOOKSIM_SRC_DIR (or the BOOKSIM_SRC_DIR env var, which
# build/simai_booksim/build.sh forwards) always wins.
if(NOT DEFINED BOOKSIM_SRC_DIR)
  set(_booksim_candidates
      "${PROJECT_SOURCE_DIR}/../../../../../booksim2/src"
      "${PROJECT_SOURCE_DIR}/../../../../../booksim2/booksim2/src")
  foreach(_c ${_booksim_candidates})
    get_filename_component(_abs "${_c}" ABSOLUTE)
    if(EXISTS "${_abs}/CMakeLists.txt")
      set(BOOKSIM_SRC_DIR "${_abs}")
      break()
    endif()
  endforeach()
endif()
if(NOT DEFINED BOOKSIM_SRC_DIR)
  message(FATAL_ERROR
    "BookSim sources not found. Looked for src/CMakeLists.txt under ${_booksim_candidates}. "
    "Set BOOKSIM_SRC_DIR=/path/to/booksim2/src (env var or -D) to override.")
endif()
get_filename_component(BOOKSIM_SRC_DIR "${BOOKSIM_SRC_DIR}" ABSOLUTE)
if(NOT EXISTS "${BOOKSIM_SRC_DIR}/CMakeLists.txt")
  message(FATAL_ERROR
    "BOOKSIM_SRC_DIR=${BOOKSIM_SRC_DIR} has no CMakeLists.txt. "
    "It should point at booksim2's src/ directory.")
endif()
message(STATUS "BookSim sources: ${BOOKSIM_SRC_DIR}")
add_subdirectory("${BOOKSIM_SRC_DIR}" booksim)

add_executable(SimAI_booksim ${SOURCES} ${HEADERS})

# booksim is PUBLIC-exporting its own include dirs, so "engine/booksim_engine.hpp"
# resolves without repeating them here.
target_link_libraries(SimAI_booksim AstraSim booksim)
