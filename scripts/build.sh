#!/bin/bash

SCRIPT_DIR=$(dirname "$(realpath $0)")
ROOT_DIR=$(realpath "${SCRIPT_DIR:?}"/..)
NS3_DIR="${ROOT_DIR:?}"/ns-3-alibabacloud
SIMAI_DIR="${ROOT_DIR:?}"/astra-sim-alibabacloud
SOURCE_NS3_BIN_DIR="${SIMAI_DIR:?}"/extern/network_backend/ns3-interface/simulation/build/scratch/ns3.36.1-AstraSimNetwork-debug
SOURCE_ANA_BIN_DIR="${SIMAI_DIR:?}"/build/simai_analytical/build/simai_analytical/SimAI_analytical
SOURCE_PHY_BIN_DIR="${SIMAI_DIR:?}"/build/simai_phy/build/simai_phynet/SimAI_phynet
SOURCE_BOOKSIM_BIN_DIR="${SIMAI_DIR:?}"/build/simai_booksim/build/simai_booksim/SimAI_booksim

TARGET_BIN_DIR="${SCRIPT_DIR:?}"/../bin
function compile {
    local option="$1" 
    case "$option" in
    "ns3")
        mkdir -p "${TARGET_BIN_DIR:?}"
        rm -rf "${SIMAI_DIR:?}"/extern/network_backend/ns3-interface/
        if [ -L "${TARGET_BIN_DIR:?}/SimAI_simulator" ]; then
            rm -rf "${TARGET_BIN_DIR:?}"/SimAI_simulator
        fi
        mkdir -p "${SIMAI_DIR:?}"/extern/network_backend/ns3-interface
        cp -r "${NS3_DIR:?}"/* "${SIMAI_DIR:?}"/extern/network_backend/ns3-interface
        cd "${SIMAI_DIR:?}"
        ./build.sh -lr ns3
        ./build.sh -c ns3    
        ln -s "${SOURCE_NS3_BIN_DIR:?}" "${TARGET_BIN_DIR:?}"/SimAI_simulator;;
    "phy")
        mkdir -p "${TARGET_BIN_DIR:?}"
        if [ -L "${TARGET_BIN_DIR:?}/SimAI_phynet" ]; then
            rm -rf "${TARGET_BIN_DIR:?}"/SimAI_phynet
        fi
        cd "${SIMAI_DIR:?}"
        ./build.sh -lr phy
        ./build.sh -c phy 
        ln -s "${SOURCE_PHY_BIN_DIR:?}" "${TARGET_BIN_DIR:?}"/SimAI_phynet;;
    "analytical")
        mkdir -p "${TARGET_BIN_DIR:?}"
        mkdir -p "${ROOT_DIR:?}"/results
        if [ -L "${TARGET_BIN_DIR:?}/SimAI_analytical" ]; then
            rm -rf "${TARGET_BIN_DIR:?}"/SimAI_analytical
        fi
        cd "${SIMAI_DIR:?}"
        ./build.sh -lr analytical
        ./build.sh -c analytical 
        ln -s "${SOURCE_ANA_BIN_DIR:?}" "${TARGET_BIN_DIR:?}"/SimAI_analytical;;
    "booksim")
        # Install only after every step succeeded; on any failure leave
        # bin/SimAI_booksim exactly as it was and exit nonzero.
        mkdir -p "${TARGET_BIN_DIR:?}" "${ROOT_DIR:?}"/results || exit 1
        cd "${SIMAI_DIR:?}" || exit 1
        if ! bash ./build.sh -lr booksim; then
            echo "build.sh: cleaning the booksim build failed; bin/SimAI_booksim left unchanged" >&2
            exit 1
        fi
        if ! bash ./build.sh -c booksim; then
            echo "build.sh: building SimAI_booksim failed; bin/SimAI_booksim left unchanged" >&2
            exit 1
        fi
        if [ ! -x "${SOURCE_BOOKSIM_BIN_DIR:?}" ]; then
            echo "build.sh: ${SOURCE_BOOKSIM_BIN_DIR} was not built; bin/SimAI_booksim left unchanged" >&2
            exit 1
        fi
        # Replace whatever is there -- an old symlink or a stale regular file
        # copied in by hand -- so bin/SimAI_booksim always runs this build.
        ln -sfn "${SOURCE_BOOKSIM_BIN_DIR:?}" "${TARGET_BIN_DIR:?}"/SimAI_booksim || exit 1;;
    esac
}

function cleanup_build {
    local option="$1"
    case "$option" in
    "ns3")
        if [ -L "${TARGET_BIN_DIR:?}/SimAI_simulator" ]; then
            rm -rf "${TARGET_BIN_DIR:?}"/SimAI_simulator
        fi
        rm -rf "${SIMAI_DIR:?}"/extern/network_backend/ns3-interface/
        cd "${SIMAI_DIR:?}"
        ./build.sh -lr ns3;;
    "phy")
        if [ -L "${TARGET_BIN_DIR:?}/SimAI_phynet" ]; then
            rm -rf "${TARGET_BIN_DIR:?}"/SimAI_phynet
        fi
        cd "${SIMAI_DIR:?}"
        ./build.sh -lr phy;;
    "analytical")
        if [ -L "${TARGET_BIN_DIR:?}/SimAI_analytical" ]; then
            rm -rf "${TARGET_BIN_DIR:?}"/SimAI_analytical
        fi
        cd "${SIMAI_DIR:?}"
        ./build.sh -lr analytical;;
    "booksim")
        if [ -L "${TARGET_BIN_DIR:?}/SimAI_booksim" ] || [ -f "${TARGET_BIN_DIR:?}/SimAI_booksim" ]; then
            rm -f "${TARGET_BIN_DIR:?}"/SimAI_booksim
        fi
        cd "${SIMAI_DIR:?}"
        bash ./build.sh -lr booksim;;
    esac
}

# Main Script
case "$1" in
-l|--clean)
    cleanup_build "$2";;
-c|--compile)
    compile "$2";;
-h|--help|*)
    printf -- "help message\n"
    printf -- "-c|--compile mode supported ns3/phy/analytical/booksim  (example:./build.sh -c ns3)\n"
    printf -- "  booksim: set BOOKSIM_SRC_DIR=/path/to/booksim2/src if booksim2 is not a sibling of SimAI/\n"
    printf -- "-l|--clean  (example:./build.sh -l ns3)\n"
    printf -- "-lr|--clean-result mode  (example:./build.sh -lr ns3)\n"
esac