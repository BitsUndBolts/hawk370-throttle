#!/bin/sh
# Builds and runs the host-side tests (needs g++ or clang++).
# Usage, from the sketch folder:  sh test/run_tests.sh
set -e
cd "$(dirname "$0")/.."
CXX=${CXX:-g++}
mkdir -p test/build
$CXX -std=c++17 -O2 -Wall -Wextra -o test/build/pattern_test test/pattern_test.cpp hawk_pattern.cpp -lm
./test/build/pattern_test
