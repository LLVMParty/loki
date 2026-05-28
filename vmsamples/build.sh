#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
BUILD_DIR="$SCRIPT_DIR/build"
CXX=${CXX:-/llvm/bin/clang++}

if [ ! -x "$CXX" ]; then
  CXX=${CXX_FALLBACK:-clang++}
fi

mkdir -p "$BUILD_DIR"

echo "--- building libvm_test_suite.so (O1) ---"
"$CXX" -std=c++17 -O1 -fPIC -shared "$SCRIPT_DIR/vm_test_suite.cpp" \
  -Wl,-Map,"$BUILD_DIR/libvm_test_suite.map" \
  -o "$BUILD_DIR/libvm_test_suite.so"

echo "--- building libvm_test_suite_loki.so (Loki VM uop + native CFG stitching) ---"
"$SCRIPT_DIR/build_loki_virtualized.sh"

echo "--- building check_vm_test_suite (O2) ---"
"$CXX" -std=c++17 -O2 "$SCRIPT_DIR/check_vm_test_suite.cpp" -ldl \
  -o "$BUILD_DIR/check_vm_test_suite"

echo
echo "built: $BUILD_DIR/libvm_test_suite.so"
echo "built: $BUILD_DIR/libvm_test_suite.map"
echo "built: $BUILD_DIR/libvm_test_suite_loki.so"
echo "built: $BUILD_DIR/libvm_test_suite_loki.map"
echo "built: $BUILD_DIR/check_vm_test_suite"
echo
echo "run:   $BUILD_DIR/check_vm_test_suite $BUILD_DIR/libvm_test_suite.so"
echo "run:   $BUILD_DIR/check_vm_test_suite $BUILD_DIR/libvm_test_suite_loki.so"
