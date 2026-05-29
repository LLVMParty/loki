#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
LOKI_DIR=$(cd -- "$SCRIPT_DIR/.." && pwd)
CXX=${CXX:-/llvm/bin/clang++}

INPUT_BC=""
SOURCE=""
OUTPUT=""
WORK_DIR=""
UOP_BC=""
VIRTUALIZE_LOG=""
VIRTUALIZED_BC=""
REBUILD_UOP_VM=0
KEEP_EVAL=0
MAX_PROCESSES=${LOKI_MAX_PROCESSES:-1}
VERIFY_ROUNDS=${LOKI_VERIFY_ROUNDS:-0}
OBFUSCATE_FLAGS=${LOKI_OBFUSCATE_FLAGS---nomba --nosuperopt}
TARGET_ARGS=()
CXXFLAGS=()
LDFLAGS=()

usage() {
  cat <<'EOF'
Usage: loki-virtualize.sh (--input input.bc | --source input.cpp) --output output.so [options]

Target selection options passed to virtualize-uops:
  --annotation NAME          virtualize annotated functions (default: loki_virtualize)
  --no-annotation           disable annotation selection
  --function NAME           virtualize a named function; may be repeated
  --exported-functions      virtualize defined externally visible functions except main
  --all-functions           virtualize every defined non-intrinsic function

Build options:
  --work-dir DIR            temporary/cache directory (default: output dir/loki-virtualize)
  --uop-bc PATH             cached obfuscated uop VM bitcode path
  --virtualize-log PATH     virtualize-uops log path
  --virtualized-bc PATH     transformed output bitcode path
  --rebuild-uop-vm          regenerate the obfuscated uop VM bitcode
  --keep-eval               keep the obfuscator eval directory
  --cxxflag FLAG            extra compile flag for --source; may be repeated
  --ldflag FLAG             extra linker flag; may be repeated

Environment:
  CXX                       clang++ path (default: /llvm/bin/clang++)
  LOKI_MAX_PROCESSES        obfuscator process cap (default: 1)
  LOKI_VERIFY_ROUNDS        obfuscator verification rounds (default: 0)
  LOKI_OBFUSCATE_FLAGS      extra obfuscate.py flags (default: --nomba --nosuperopt)
EOF
}

while [ "$#" -gt 0 ]; do
  case "$1" in
    --input)
      INPUT_BC=${2:?--input requires a path}
      shift 2
      ;;
    --source)
      SOURCE=${2:?--source requires a path}
      shift 2
      ;;
    --output)
      OUTPUT=${2:?--output requires a path}
      shift 2
      ;;
    --work-dir)
      WORK_DIR=${2:?--work-dir requires a path}
      shift 2
      ;;
    --uop-bc)
      UOP_BC=${2:?--uop-bc requires a path}
      shift 2
      ;;
    --virtualize-log)
      VIRTUALIZE_LOG=${2:?--virtualize-log requires a path}
      shift 2
      ;;
    --virtualized-bc)
      VIRTUALIZED_BC=${2:?--virtualized-bc requires a path}
      shift 2
      ;;
    --rebuild-uop-vm)
      REBUILD_UOP_VM=1
      shift
      ;;
    --keep-eval)
      KEEP_EVAL=1
      shift
      ;;
    --cxxflag)
      CXXFLAGS+=("${2:?--cxxflag requires a value}")
      shift 2
      ;;
    --ldflag)
      LDFLAGS+=("${2:?--ldflag requires a value}")
      shift 2
      ;;
    --annotation)
      TARGET_ARGS+=(--annotation "${2:?--annotation requires a value}")
      shift 2
      ;;
    --no-annotation|--exported-functions|--all-functions)
      TARGET_ARGS+=("$1")
      shift
      ;;
    --function)
      TARGET_ARGS+=(--function "${2:?--function requires a value}")
      shift 2
      ;;
    --help|-h)
      usage
      exit 0
      ;;
    *)
      echo "unknown argument: $1" >&2
      usage >&2
      exit 1
      ;;
  esac
done

if [ -z "$OUTPUT" ] || { [ -z "$INPUT_BC" ] && [ -z "$SOURCE" ]; }; then
  usage >&2
  exit 1
fi

if [ -n "$INPUT_BC" ] && [ -n "$SOURCE" ]; then
  echo "use either --input or --source, not both" >&2
  exit 1
fi

if [ -n "$INPUT_BC" ] && [ ! -f "$INPUT_BC" ]; then
  echo "input bitcode not found: $INPUT_BC" >&2
  exit 1
fi

if [ -n "$SOURCE" ] && [ ! -f "$SOURCE" ]; then
  echo "source not found: $SOURCE" >&2
  exit 1
fi

if [ ! -x "$CXX" ]; then
  CXX=${CXX_FALLBACK:-clang++}
fi

OUTPUT_PARENT=$(dirname -- "$OUTPUT")
mkdir -p "$OUTPUT_PARENT"
OUTPUT_DIR=$(cd -- "$OUTPUT_PARENT" && pwd)
OUTPUT_NAME=$(basename -- "$OUTPUT")

if [ -z "$WORK_DIR" ]; then
  WORK_DIR="$OUTPUT_DIR/loki-virtualize"
fi
if [ -z "$UOP_BC" ]; then
  UOP_BC="$SCRIPT_DIR/bin/uop_dispatch_obf.bc"
fi
if [ -z "$VIRTUALIZE_LOG" ]; then
  VIRTUALIZE_LOG="$WORK_DIR/virtualize-uops.log"
fi

mkdir -p "$WORK_DIR" "$(dirname -- "$UOP_BC")" "$(dirname -- "$VIRTUALIZE_LOG")"

if [ -n "$SOURCE" ]; then
  INPUT_BC="$WORK_DIR/$(basename -- "${SOURCE%.*}").bc"
  "$CXX" -std=c++17 -O1 -fPIC -c -emit-llvm \
    "${CXXFLAGS[@]}" \
    "$SOURCE" -o "$INPUT_BC"
fi

VIRTUALIZE_UOPS="$SCRIPT_DIR/bin/virtualize-uops"
UOP_SOURCE="$SCRIPT_DIR/src/uop_dispatch.cpp"
TEMPLATE_SOURCE="$SCRIPT_DIR/src/template.cpp"
UOP_EVAL_DIR="$WORK_DIR/uop-dispatch-eval"
if [ -z "$VIRTUALIZED_BC" ]; then
  VIRTUALIZED_BC="$WORK_DIR/${OUTPUT_NAME%.so}.uops.bc"
fi
mkdir -p "$(dirname -- "$VIRTUALIZED_BC")"

CONFIG_FILE="$LOKI_DIR/obfuscator/vm_alu/src/config.rs"
CONFIG_BACKUP=$(mktemp)
cp "$CONFIG_FILE" "$CONFIG_BACKUP"
restore_config() {
  cp "$CONFIG_BACKUP" "$CONFIG_FILE" 2>/dev/null || true
  rm -f "$CONFIG_BACKUP"
}
trap restore_config EXIT

if [ ! -x "$VIRTUALIZE_UOPS" ] || [ "$SCRIPT_DIR/src/virtualize_uops.cpp" -nt "$VIRTUALIZE_UOPS" ] || [ "$TEMPLATE_SOURCE" -nt "$SCRIPT_DIR/bin/template.bc" ]; then
  (cd "$SCRIPT_DIR" && ./build.sh)
fi

if [ "$REBUILD_UOP_VM" -eq 1 ] || [ ! -f "$UOP_BC" ] || [ "$UOP_SOURCE" -nt "$UOP_BC" ] || [ "$TEMPLATE_SOURCE" -nt "$UOP_BC" ]; then
  rm -rf "$UOP_EVAL_DIR"
  (cd "$LOKI_DIR" && python3 obfuscate.py \
    --testcase-path "$UOP_SOURCE" \
    --instances 1 \
    --verification-rounds "$VERIFY_ROUNDS" \
    --max-processes "$MAX_PROCESSES" \
    --debug \
    $OBFUSCATE_FLAGS \
    "$UOP_EVAL_DIR")
  cp "$UOP_EVAL_DIR/workdirs/uop_dispatch/instances/vm_alu000/obf.bc" "$UOP_BC"
  if [ "$KEEP_EVAL" -eq 0 ]; then
    rm -rf "$UOP_EVAL_DIR"
  fi
fi

"$VIRTUALIZE_UOPS" "${TARGET_ARGS[@]}" "$INPUT_BC" "$VIRTUALIZED_BC" >"$VIRTUALIZE_LOG" 2>&1
cat "$VIRTUALIZE_LOG"

"$CXX" -std=c++17 -O1 -fPIC -shared \
  "$VIRTUALIZED_BC" "$UOP_BC" \
  -Wl,-Bsymbolic -Wl,--no-undefined \
  "${LDFLAGS[@]}" \
  -o "$OUTPUT"

echo "built: $OUTPUT"
echo "using uop VM: $UOP_BC"
echo "virtualized bitcode: $VIRTUALIZED_BC"
