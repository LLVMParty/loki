#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
ROOT_DIR=$(cd -- "$SCRIPT_DIR/.." && pwd)
BUILD_DIR="$SCRIPT_DIR/build"
CXX=${CXX:-/llvm/bin/clang++}
LOKI_DIR="$ROOT_DIR/loki"
UOP_WORK_DIR="$BUILD_DIR/loki-uop"
UOP_BC=${LOKI_UOP_BC:-$UOP_WORK_DIR/obf.bc}

if [ ! -x "$CXX" ]; then
  CXX=${CXX_FALLBACK:-clang++}
fi

mkdir -p "$BUILD_DIR" "$UOP_WORK_DIR" "$(dirname -- "$UOP_BC")"

INPUT_BC="$UOP_WORK_DIR/vm_test_suite.bc"

"$CXX" -std=c++17 -O1 -fPIC -c -emit-llvm \
  "$SCRIPT_DIR/vm_test_suite.cpp" -o "$INPUT_BC"

"$LOKI_DIR/translator/loki-virtualize.sh" \
  --input "$INPUT_BC" \
  --output "$BUILD_DIR/libvm_test_suite_loki.so" \
  --work-dir "$UOP_WORK_DIR" \
  --uop-bc "$UOP_BC" \
  --virtualized-bc "$UOP_WORK_DIR/vm_test_suite.uops.bc" \
  --annotation loki_virtualize \
  --ldflag "-Wl,-Map,$BUILD_DIR/libvm_test_suite_loki.map"
