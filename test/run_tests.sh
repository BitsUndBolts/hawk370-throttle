#!/bin/sh
# Builds and runs the host-side tests (needs g++ or clang++).
# Usage, from the sketch folder:  sh test/run_tests.sh
# The preset test also needs ArduinoJson; set ARDUINOJSON_SRC to its src/
# folder if it is not in one of the usual Arduino library locations.
set -e
cd "$(dirname "$0")/.."
CXX=${CXX:-g++}
mkdir -p test/build

$CXX -std=c++17 -O2 -Wall -Wextra -o test/build/pattern_test test/pattern_test.cpp hawk_pattern.cpp -lm
./test/build/pattern_test

for d in "$ARDUINOJSON_SRC" "$HOME/Arduino/libraries/ArduinoJson/src" \
         "$HOME/Documents/Arduino/libraries/ArduinoJson/src"; do
  if [ -n "$d" ] && [ -f "$d/ArduinoJson.h" ]; then AJ="$d"; break; fi
done
if [ -z "$AJ" ]; then
  echo "ArduinoJson not found: skipping the preset test (set ARDUINOJSON_SRC)"
  exit 0
fi
# test/shims provides Arduino.h, LittleFS.h and a String stand-in
$CXX -std=c++17 -O2 -Itest/shims -I"$AJ" -include test/shims/ArduinoJsonShim.h \
     -o test/build/presets_test test/presets_test.cpp hawk_presets.cpp -lpthread
./test/build/presets_test
