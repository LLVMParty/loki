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
UOP_MAP=""
VIRTUALIZE_LOG=""
VIRTUALIZED_BC=""
BATCH_CPP=""
REBUILD_UOP_VM=0
REGENERATE_UOP_MAP=0
UOP_MAP_SEED=""
KEEP_EVAL=0
ENABLE_BATCH=1
MAX_BATCH_NODES=12
INLINE_VM=0
LOCALIZE_LOKI_SYMBOLS=0
MAX_PROCESSES=${LOKI_MAX_PROCESSES:-1}
VERIFY_ROUNDS=${LOKI_VERIFY_ROUNDS:-0}
OBFUSCATE_FLAGS=${LOKI_OBFUSCATE_FLAGS---nomba --nosuperopt}
TARGET_ARGS=()
CXXFLAGS=()
LDFLAGS=()
EXPORT_SYMBOLS=()

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
  --uop-map PATH            stable uop opcode/encoding map path
  --regenerate-uop-map      replace the uop map and rebuild the uop VM
  --uop-map-seed SEED       deterministic seed used when creating/regenerating a map
  --virtualize-log PATH     virtualize-uops log path
  --virtualized-bc PATH     transformed output bitcode path
  --rebuild-uop-vm          regenerate the obfuscated uop VM bitcode
  --keep-eval               keep the obfuscator eval directory
  --no-batch                disable target-specific expression batching
  --max-batch-nodes N       maximum supported instructions per batch (default: 12)
  --inline-vm               experimental: llvm-link VM bitcode and run always-inline
  --hide-loki-symbols       localize Loki VM helper symbols in the output shared library
  --keep-loki-symbols       compatibility no-op; symbols are kept by default
  --export-symbol NAME      with --hide-loki-symbols, force a symbol into the export list; may be repeated
  --cxxflag FLAG            extra compile flag for --source; may be repeated
  --ldflag FLAG             extra linker flag; may be repeated

Environment:
  CXX                       clang++ path (default: /llvm/bin/clang++)
  LLVM_LINK                 llvm-link path
  OPT                       opt path
  LLVM_NM                   llvm-nm path
  OBJCOPY                   objcopy path
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
    --uop-map)
      UOP_MAP=${2:?--uop-map requires a path}
      shift 2
      ;;
    --regenerate-uop-map)
      REGENERATE_UOP_MAP=1
      REBUILD_UOP_VM=1
      shift
      ;;
    --uop-map-seed)
      UOP_MAP_SEED=${2:?--uop-map-seed requires a value}
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
    --no-batch)
      ENABLE_BATCH=0
      shift
      ;;
    --max-batch-nodes)
      MAX_BATCH_NODES=${2:?--max-batch-nodes requires a value}
      shift 2
      ;;
    --inline-vm)
      INLINE_VM=1
      shift
      ;;
    --hide-loki-symbols)
      LOCALIZE_LOKI_SYMBOLS=1
      shift
      ;;
    --keep-loki-symbols)
      LOCALIZE_LOKI_SYMBOLS=0
      shift
      ;;
    --export-symbol)
      EXPORT_SYMBOLS+=("${2:?--export-symbol requires a value}")
      shift 2
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

if ! [[ "$MAX_BATCH_NODES" =~ ^[0-9]+$ ]] || [ "$MAX_BATCH_NODES" -lt 2 ] || [ "$MAX_BATCH_NODES" -gt 64 ]; then
  echo "--max-batch-nodes must be in [2, 64]" >&2
  exit 1
fi

resolve_tool() {
  local requested=$1
  local fallback=$2
  if [ -n "$requested" ] && [ -x "$requested" ]; then
    printf '%s\n' "$requested"
    return
  fi
  if [ -x "$fallback" ]; then
    printf '%s\n' "$fallback"
    return
  fi
  if command -v "$(basename -- "$fallback")" >/dev/null 2>&1; then
    command -v "$(basename -- "$fallback")"
    return
  fi
  printf '%s\n' "$requested"
}

CXX_RESOLVED=$(command -v "$CXX" 2>/dev/null || true)
if [ -z "$CXX_RESOLVED" ]; then
  CXX_RESOLVED=$CXX
fi
CXX_DIR=$(cd -- "$(dirname -- "$CXX_RESOLVED")" 2>/dev/null && pwd || dirname -- "$CXX_RESOLVED")
LLVM_LINK=$(resolve_tool "${LLVM_LINK:-}" "$CXX_DIR/llvm-link")
OPT=$(resolve_tool "${OPT:-}" "$CXX_DIR/opt")
LLVM_NM=$(resolve_tool "${LLVM_NM:-}" "$CXX_DIR/llvm-nm")
OBJCOPY=$(resolve_tool "${OBJCOPY:-}" "$(command -v objcopy 2>/dev/null || echo objcopy)")

OUTPUT_PARENT=$(dirname -- "$OUTPUT")
mkdir -p "$OUTPUT_PARENT"
OUTPUT_DIR=$(cd -- "$OUTPUT_PARENT" && pwd)
OUTPUT_NAME=$(basename -- "$OUTPUT")

if [ -z "$WORK_DIR" ]; then
  WORK_DIR="$OUTPUT_DIR/loki-virtualize"
fi
if [ -z "$UOP_BC" ]; then
  UOP_BC="$WORK_DIR/uop_dispatch_obf.bc"
fi
if [ -z "$UOP_MAP" ]; then
  UOP_MAP="$SCRIPT_DIR/src/uop_dispatch.map"
fi
if [ -z "$VIRTUALIZE_LOG" ]; then
  VIRTUALIZE_LOG="$WORK_DIR/virtualize-uops.log"
fi

mkdir -p "$WORK_DIR" "$(dirname -- "$UOP_BC")" "$(dirname -- "$UOP_MAP")" "$(dirname -- "$VIRTUALIZE_LOG")"
WORK_DIR=$(cd -- "$WORK_DIR" && pwd)
UOP_BC_DIR=$(cd -- "$(dirname -- "$UOP_BC")" && pwd)
UOP_BC="$UOP_BC_DIR/$(basename -- "$UOP_BC")"
UOP_MAP_DIR=$(cd -- "$(dirname -- "$UOP_MAP")" && pwd)
UOP_MAP="$UOP_MAP_DIR/$(basename -- "$UOP_MAP")"
VIRTUALIZE_LOG_DIR=$(cd -- "$(dirname -- "$VIRTUALIZE_LOG")" && pwd)
VIRTUALIZE_LOG="$VIRTUALIZE_LOG_DIR/$(basename -- "$VIRTUALIZE_LOG")"

if [ -n "$SOURCE" ]; then
  INPUT_BC="$WORK_DIR/$(basename -- "${SOURCE%.*}").bc"
  "$CXX" -std=c++17 -O1 -fPIC -c -emit-llvm \
    "${CXXFLAGS[@]}" \
    "$SOURCE" -o "$INPUT_BC"
fi

VIRTUALIZE_UOPS="$SCRIPT_DIR/bin/virtualize-uops"
UOP_SOURCE="$WORK_DIR/uop_dispatch.cpp"
TEMPLATE_SOURCE="$SCRIPT_DIR/src/template.cpp"
UOP_EVAL_DIR="$WORK_DIR/uop-dispatch-eval"
if [ -z "$BATCH_CPP" ]; then
  BATCH_CPP="$WORK_DIR/uop_batches.cpp.inc"
fi
LINKED_BC="$WORK_DIR/${OUTPUT_NAME%.so}.linked.bc"
INLINED_BC="$WORK_DIR/${OUTPUT_NAME%.so}.linked.inline.bc"
EXPORTS_FILE="$WORK_DIR/${OUTPUT_NAME%.so}.exports"
VERSION_SCRIPT="$WORK_DIR/${OUTPUT_NAME%.so}.version"
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

GENERATOR_ARGS=(--map "$UOP_MAP" --output "$UOP_SOURCE")
if [ "$REGENERATE_UOP_MAP" -eq 1 ]; then
  GENERATOR_ARGS+=(--force-map)
fi
if [ -n "$UOP_MAP_SEED" ]; then
  GENERATOR_ARGS+=(--seed "$UOP_MAP_SEED")
fi
UOP_GENERATOR="$SCRIPT_DIR/generate_uop_dispatch.py"
python3 "$UOP_GENERATOR" "${GENERATOR_ARGS[@]}"

VIRTUALIZE_ARGS=(--op-map "$UOP_MAP")
if [ "$ENABLE_BATCH" -eq 1 ]; then
  VIRTUALIZE_ARGS+=(--batch-cpp "$BATCH_CPP" --max-batch-nodes "$MAX_BATCH_NODES")
else
  VIRTUALIZE_ARGS+=(--no-batch)
  if [ ! -f "$BATCH_CPP" ] || [ -s "$BATCH_CPP" ]; then
    : >"$BATCH_CPP"
  fi
fi
"$VIRTUALIZE_UOPS" "${VIRTUALIZE_ARGS[@]}" "${TARGET_ARGS[@]}" "$INPUT_BC" "$VIRTUALIZED_BC" >"$VIRTUALIZE_LOG" 2>&1
cat "$VIRTUALIZE_LOG"

FINAL_GENERATOR_ARGS=(--map "$UOP_MAP" --output "$UOP_SOURCE")
if [ "$ENABLE_BATCH" -eq 1 ]; then
  FINAL_GENERATOR_ARGS+=(--batch-cpp "$BATCH_CPP")
fi
python3 "$UOP_GENERATOR" "${FINAL_GENERATOR_ARGS[@]}"

if [ "$REBUILD_UOP_VM" -eq 1 ] || [ ! -f "$UOP_BC" ] || [ "$UOP_GENERATOR" -nt "$UOP_BC" ] || [ "$UOP_MAP" -nt "$UOP_BC" ] || [ "$BATCH_CPP" -nt "$UOP_BC" ] || [ "$TEMPLATE_SOURCE" -nt "$UOP_BC" ]; then
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

LINK_INPUTS=("$VIRTUALIZED_BC" "$UOP_BC")
if [ "$INLINE_VM" -eq 1 ]; then
  if [ ! -x "$LLVM_LINK" ] || [ ! -x "$OPT" ]; then
    echo "llvm-link/opt unavailable; omit --inline-vm or set LLVM_LINK/OPT" >&2
    exit 1
  fi
  "$LLVM_LINK" "$VIRTUALIZED_BC" "$UOP_BC" -o "$LINKED_BC"
  "$OPT" -always-inline "$LINKED_BC" -o "$INLINED_BC"
  LINK_INPUTS=("$INLINED_BC")
fi

if [ "$LOCALIZE_LOKI_SYMBOLS" -eq 1 ] && [ -x "$LLVM_NM" ]; then
  {
    "$LLVM_NM" --defined-only --extern-only "$VIRTUALIZED_BC" \
      | awk '{print $3}' \
      | grep -Ev '^(|main|target_function|loki_vm_.*|vm_alu.*)$' || true
    printf '%s\n' "${EXPORT_SYMBOLS[@]}"
  } | awk 'NF && !seen[$0]++' >"$EXPORTS_FILE"
  {
    echo '{'
    echo '  global:'
    if [ -s "$EXPORTS_FILE" ]; then
      sed 's/.*/    &;/' "$EXPORTS_FILE"
    fi
    echo '  local:'
    echo '    *;'
    echo '};'
  } >"$VERSION_SCRIPT"
  LDFLAGS+=("-Wl,--version-script,$VERSION_SCRIPT")
fi

"$CXX" -std=c++17 -O1 -fPIC -shared \
  "${LINK_INPUTS[@]}" \
  -Wl,-Bsymbolic -Wl,--no-undefined \
  "${LDFLAGS[@]}" \
  -o "$OUTPUT"

if [ "$LOCALIZE_LOKI_SYMBOLS" -eq 1 ] && [ -x "$OBJCOPY" ]; then
  "$OBJCOPY" --wildcard \
    --localize-symbol 'loki_vm_*' \
    --localize-symbol 'vm_alu*' \
    --localize-symbol 'vm_setup' \
    --localize-symbol 'vm_exit' \
    --localize-symbol 'handler_table' \
    --localize-symbol 'context' \
    --localize-symbol 'argument_*' \
    --localize-symbol 'bytecode' \
    --localize-symbol 'parse_input' \
    --localize-symbol 'main' \
    "$OUTPUT"
fi

echo "built: $OUTPUT"
echo "using uop VM: $UOP_BC"
echo "using uop map: $UOP_MAP"
if [ "$ENABLE_BATCH" -eq 1 ]; then
  echo "using batch semantics: $BATCH_CPP"
fi
echo "virtualized bitcode: $VIRTUALIZED_BC"
if [ "$INLINE_VM" -eq 1 ]; then
  echo "linked/inlined bitcode: $INLINED_BC"
fi
