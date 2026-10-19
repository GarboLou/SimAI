#!/bin/bash
# Unit tests for the BookSim frontend's standalone helpers. Not part of the
# SimAI_booksim build (its CMakeLists globs only the frontend directory).
#   bash astra-sim/network_frontend/booksim/tests/run_tests.sh
set -e
DIR=$(dirname "$(realpath "$0")")
OUT=$(mktemp -d)
trap 'rm -rf "$OUT"' EXIT
CXX=${CXX:-g++}
$CXX -std=c++11 -Wall -Wextra -Werror -O1 $EXTRA_CXXFLAGS \
    -o "$OUT/test_send_latency" "$DIR/test_send_latency.cc"
"$OUT/test_send_latency"
