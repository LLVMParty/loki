#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
ROOT_DIR=$(cd -- "$SCRIPT_DIR/.." && pwd)
BUILD_DIR="$SCRIPT_DIR/build"
LIB="$BUILD_DIR/libvm_test_suite_loki.so"
CHECKER="$BUILD_DIR/check_vm_test_suite"
LOG="$BUILD_DIR/check_loki_vm_test_suite.log"
CXX=${CXX:-/llvm/bin/clang++}

if [ ! -x "$CXX" ]; then
  CXX=${CXX_FALLBACK:-clang++}
fi

"$SCRIPT_DIR/build.sh"

VIRT_LOG="$BUILD_DIR/loki-uop/virtualize-uops.log"
if [ -f "$VIRT_LOG" ]; then
  virtualized_functions=$(grep -c '^virtualized ' "$VIRT_LOG")
  if [ "$virtualized_functions" -ne 23 ]; then
    echo "expected virtualize-uops to process 23 functions, found $virtualized_functions" >&2
    exit 1
  fi
  if grep -q '^virtualized 0 uops' "$VIRT_LOG"; then
    echo "at least one exported function had zero Loki uops" >&2
    exit 1
  fi
fi

VIRTUALIZED_BC="$BUILD_DIR/loki-uop/vm_test_suite.uops.bc"
if [ -x /llvm/bin/llvm-dis ] && [ -f "$VIRTUALIZED_BC" ] && command -v python3 >/dev/null 2>&1; then
  /llvm/bin/llvm-dis "$VIRTUALIZED_BC" -o "$BUILD_DIR/loki-uop/vm_test_suite.uops.ll"
  python3 - "$BUILD_DIR/loki-uop/vm_test_suite.uops.ll" <<'PY'
import re
import sys
current = None
counts = {}
residual = []
for line in open(sys.argv[1], encoding="utf-8"):
    m = re.match(r"define .* @(vm\d\d_[^(]+)\(", line)
    if m:
        current = m.group(1)
        counts[current] = 0
        continue
    if current and line.startswith("}"):
        current = None
        continue
    if not current:
        continue
    if "call i64 @loki_vm_enter" in line:
        counts[current] += 1
    if "loki.enc" in line:
        continue
    if re.search(r"= icmp ", line) or re.search(r"= select ", line) or re.search(r"= (add|sub|mul|xor|and|or|shl|lshr|ashr|udiv|sdiv|urem|srem) ", line):
        residual.append((current, line.strip()))
if len(counts) != 23:
    raise SystemExit(f"expected 23 transformed vmXX functions, found {len(counts)}")
missing = [name for name, count in counts.items() if count == 0]
if missing:
    raise SystemExit(f"functions without direct Loki VM entries: {missing}")
if residual:
    raise SystemExit("residual supported uops outside Loki VM entries: " + repr(residual[:5]))
print(f"verified {sum(counts.values())} direct Loki VM entries across {len(counts)} functions")
PY
fi

if command -v nm >/dev/null 2>&1; then
  symbols=$(nm -D --defined-only "$LIB")
  export_count=$(printf '%s\n' "$symbols" | awk '{print $3}' | grep -E '^vm[0-9][0-9]_' | wc -l)
  if [ "$export_count" -ne 23 ]; then
    echo "expected 23 exported vmXX_* symbols, found $export_count" >&2
    exit 1
  fi
  grep -q ' loki_vm_enter$' <<<"$symbols"
  grep -q ' loki_vm_call$' <<<"$symbols"
  grep -q ' vm_alu[0-9][0-9]*_rrr_generated$' <<<"$symbols"
  ! nm -u "$LIB" | grep -q 'loki_vm_'
fi

LIFTER="$ROOT_DIR/loki/translator/bin/lift_input"
if [ -x "$LIFTER" ] && command -v python3 >/dev/null 2>&1; then
  LIFT_DIR="$BUILD_DIR/loki_annotation_lift"
  rm -rf "$LIFT_DIR"
  mkdir -p "$LIFT_DIR"
  "$CXX" -c -emit-llvm --std=c++17 -O1 -Xclang -disable-llvm-passes \
    "$SCRIPT_DIR/vm_test_suite.cpp" -o "$LIFT_DIR/input_program.bc"
  "$LIFTER" "$LIFT_DIR" >"$LIFT_DIR/lift.stdout" 2>"$LIFT_DIR/lift.stderr"
  python3 - "$LIFT_DIR/loki_functions_manifest.json" <<'PY'
import json
import sys
manifest = json.load(open(sys.argv[1], encoding="utf-8"))
functions = manifest.get("functions", [])
if len(functions) != 23:
    raise SystemExit(f"expected 23 annotated functions, found {len(functions)}")
missing = [f"vm{i:02d}" for i in range(1, 24)
           if not any(entry.get("name", "").startswith(f"vm{i:02d}_") for entry in functions)]
if missing:
    raise SystemExit(f"missing annotated function prefixes: {missing}")
PY
fi

"$CHECKER" "$LIB" | tee "$LOG"
grep -q 'summary: 892 passed, 0 failed, 0 skipped' "$LOG"
