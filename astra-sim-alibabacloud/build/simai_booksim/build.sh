#!/bin/bash

# Absolue path to this script
SCRIPT_DIR=$(dirname "$(realpath $0)")

# Absolute paths to useful directories
BUILD_DIR="${SCRIPT_DIR:?}"/build/
RESULT_DIR="${SCRIPT_DIR:?}"/result/
BIN_DIR="${BUILD_DIR}"/BookSimAstra/bin/
BINARY="./BookSimAstra"

# Functions
function cleanup_build {
    rm -rf "${BUILD_DIR}"
}

function cleanup_result {
    rm -rf "${RESULT_DIR}"
}

function setup {
    mkdir -p "${BUILD_DIR}"
    mkdir -p "${RESULT_DIR}"
}

function compile {
    cd "${BUILD_DIR}" || exit
    # BOOKSIM_SRC_DIR overrides where the BookSim checkout lives. The frontend's
    # CMakeLists defaults to ../../../../../booksim2/booksim2/src relative to
    # itself, which is correct when SimAI and booksim2 are siblings; set this
    # env var if your layout differs.
    if [ -n "${BOOKSIM_SRC_DIR}" ]; then
        cmake -DUSE_BOOKSIM=TRUE -DBOOKSIM_SRC_DIR="${BOOKSIM_SRC_DIR}" ..
    else
        cmake -DUSE_BOOKSIM=TRUE ..
    fi
    make
}


# Main Script
case "$1" in
-l|--clean)
    cleanup_build;;
-lr|--clean-result)
    cleanup_build
    cleanup_result;;
-c|--compile)
    setup
    compile;;
-h|--help|*)
    echo "BookSimAstra build script."
    echo "Run $0 -c to compile.";;
esac

