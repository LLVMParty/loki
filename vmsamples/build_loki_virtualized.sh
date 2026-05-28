#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
ROOT_DIR=$(cd -- "$SCRIPT_DIR/.." && pwd)
BUILD_DIR="$SCRIPT_DIR/build"
CXX=${CXX:-/llvm/bin/clang++}
LOKI_DIR="$ROOT_DIR/loki"
UOP_WORK_DIR="$BUILD_DIR/loki-uop"
UOP_BC=${LOKI_UOP_BC:-$UOP_WORK_DIR/obf.bc}
EVAL_DIR=${LOKI_UOP_EVAL_DIR:-$UOP_WORK_DIR/eval}
VIRTUALIZE_UOPS="$LOKI_DIR/translator/bin/virtualize-uops"
MAX_PROCESSES=${LOKI_MAX_PROCESSES:-1}
VERIFY_ROUNDS=${LOKI_VERIFY_ROUNDS:-0}
OBFUSCATE_FLAGS=${LOKI_OBFUSCATE_FLAGS---nomba --nosuperopt}

if [ ! -x "$CXX" ]; then
  CXX=${CXX_FALLBACK:-clang++}
fi

mkdir -p "$BUILD_DIR" "$UOP_WORK_DIR" "$(dirname "$UOP_BC")"

CONFIG_FILE="$LOKI_DIR/obfuscator/vm_alu/src/config.rs"
CONFIG_BACKUP=$(mktemp)
cp "$CONFIG_FILE" "$CONFIG_BACKUP"
restore_config() {
  cp "$CONFIG_BACKUP" "$CONFIG_FILE" 2>/dev/null || true
  rm -f "$CONFIG_BACKUP"
}
trap restore_config EXIT

if [ ! -x "$VIRTUALIZE_UOPS" ] || [ "$LOKI_DIR/translator/src/virtualize_uops.cpp" -nt "$VIRTUALIZE_UOPS" ]; then
  (cd "$LOKI_DIR/translator" && ./build.sh)
fi

UOP_SOURCE="$LOKI_DIR/translator/src/uop_dispatch.cpp"
if [ ! -f "$UOP_BC" ] || [ "$UOP_SOURCE" -nt "$UOP_BC" ] || [ "$LOKI_DIR/translator/src/template.cpp" -nt "$UOP_BC" ]; then
  rm -rf "$EVAL_DIR"
  (cd "$LOKI_DIR" && python3 obfuscate.py \
    --testcase-path "$UOP_SOURCE" \
    --instances 1 \
    --verification-rounds "$VERIFY_ROUNDS" \
    --max-processes "$MAX_PROCESSES" \
    --debug \
    $OBFUSCATE_FLAGS \
    "$EVAL_DIR")
  cp "$EVAL_DIR/workdirs/uop_dispatch/instances/vm_alu000/obf.bc" "$UOP_BC"
fi

INPUT_BC="$UOP_WORK_DIR/vm_test_suite.bc"
VIRTUALIZED_BC="$UOP_WORK_DIR/vm_test_suite.uops.bc"
VIRTUALIZE_LOG="$UOP_WORK_DIR/virtualize-uops.log"

"$CXX" -std=c++17 -O1 -fPIC -c -emit-llvm \
  "$SCRIPT_DIR/vm_test_suite.cpp" -o "$INPUT_BC"
"$VIRTUALIZE_UOPS" "$INPUT_BC" "$VIRTUALIZED_BC" >"$VIRTUALIZE_LOG" 2>&1
cat "$VIRTUALIZE_LOG"

"$CXX" -std=c++17 -O1 -fPIC -shared \
  "$VIRTUALIZED_BC" "$UOP_BC" \
  -Wl,-Bsymbolic -Wl,--no-undefined \
  -Wl,-Map,"$BUILD_DIR/libvm_test_suite_loki.map" \
  -o "$BUILD_DIR/libvm_test_suite_loki.so"

echo "built: $BUILD_DIR/libvm_test_suite_loki.so"
echo "using: $UOP_BC"
