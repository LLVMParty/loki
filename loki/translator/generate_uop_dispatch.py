#!/usr/bin/env python3
import argparse
import random
import secrets
from pathlib import Path
from typing import Optional

OPERATIONS = [
    "add", "sub", "xor", "and", "or", "mul", "lshr", "shl", "not",
    "udiv", "urem", "eq", "iszero", "select", "addc", "xorc", "ne",
    "ult", "ule", "ugt", "uge", "slt", "sle", "sgt", "sge", "ashr",
    "sdiv", "srem",
]

ENCODING_KEYS = ["encode_op_xor", "encode_a_xor", "encode_b_xor", "encode_c_xor"]

DEFAULT_FIXED_VALUES = {
    "add": 0,
    "sub": 1,
    "xor": 2,
    "and": 3,
    "or": 4,
    "mul": 5,
    "lshr": 6,
    "shl": 7,
    "not": 8,
    "udiv": 9,
    "urem": 10,
    "eq": 11,
    "iszero": 12,
    "select": 13,
    "addc": 14,
    "xorc": 15,
    "ne": 16,
    "ult": 17,
    "ule": 18,
    "ugt": 19,
    "uge": 20,
    "slt": 21,
    "sle": 22,
    "sgt": 23,
    "sge": 24,
    "ashr": 25,
    "sdiv": 26,
    "srem": 27,
    "encode_op_xor": 0,
    "encode_a_xor": 0,
    "encode_b_xor": 0,
    "encode_c_xor": 0,
}


def parse_int(value: str) -> int:
    return int(value, 0) & ((1 << 64) - 1)


def load_map(path: Path) -> dict:
    values = dict(DEFAULT_FIXED_VALUES)
    if not path.exists():
        return values
    for raw_line in path.read_text(encoding="utf-8").splitlines():
        line = raw_line.split("#", 1)[0].strip()
        if not line:
            continue
        parts = line.split()
        if len(parts) != 2:
            raise SystemExit(f"invalid map line in {path}: {raw_line!r}")
        key, value = parts
        if key not in values:
            raise SystemExit(f"unknown uop map key in {path}: {key}")
        values[key] = parse_int(value)
    return values


def random_u64(rng) -> int:
    if rng is None:
        return secrets.randbits(64)
    return rng.getrandbits(64)


def generate_map(seed: Optional[str]) -> dict:
    rng = random.Random(seed) if seed is not None else None
    used = set()
    values = {}
    for key in OPERATIONS:
        value = 0
        while value == 0 or value in used:
            value = random_u64(rng)
        values[key] = value
        used.add(value)
    for key in ENCODING_KEYS:
        value = 0
        while value == 0:
            value = random_u64(rng)
        values[key] = value
    return values


def write_map(path: Path, values: dict) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    lines = [
        "# Loki uop dispatch map v1",
        "# Keep this file to make repeated protections use the same external uop ABI.",
        "# Regenerate it only when you intentionally want a new protected ABI.",
        "",
    ]
    for key in ENCODING_KEYS:
        lines.append(f"{key} 0x{values[key]:016x}")
    lines.append("")
    for key in OPERATIONS:
        lines.append(f"{key} 0x{values[key]:016x}")
    lines.append("")
    with path.open("w", encoding="utf-8", newline="\n") as output:
        output.write("\n".join(lines))


def c_const(values: dict, key: str) -> str:
    return f"0x{values[key]:016x}ull"


def render_cpp(values: dict) -> str:
    return f'''#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

#define LOKI_IS_ZERO64(value) (((((value) | (0ull - (value))) >> 63) ^ 1ull) & 1ull)
#define LOKI_MASK_EQ(value, key) (0ull - LOKI_IS_ZERO64((value) ^ static_cast<uint64_t>(key)))
#define LOKI_ULT64(lhs, rhs) (((~(lhs) & (rhs)) | (~((lhs) ^ (rhs)) & ((lhs) - (rhs)))) >> 63)
#define LOKI_SLT64(lhs, rhs) ((((lhs) >> 63) & ~((rhs) >> 63)) | (((((lhs) >> 63) ^ ((rhs) >> 63)) ^ 1ull) & LOKI_ULT64((lhs), (rhs))))

static constexpr uint64_t kEncodeOpXor = {c_const(values, "encode_op_xor")};
static constexpr uint64_t kEncodeAXor = {c_const(values, "encode_a_xor")};
static constexpr uint64_t kEncodeBXor = {c_const(values, "encode_b_xor")};
static constexpr uint64_t kEncodeCXor = {c_const(values, "encode_c_xor")};

static constexpr uint64_t kOpAdd = {c_const(values, "add")};
static constexpr uint64_t kOpSub = {c_const(values, "sub")};
static constexpr uint64_t kOpXor = {c_const(values, "xor")};
static constexpr uint64_t kOpAnd = {c_const(values, "and")};
static constexpr uint64_t kOpOr = {c_const(values, "or")};
static constexpr uint64_t kOpMul = {c_const(values, "mul")};
static constexpr uint64_t kOpLShr = {c_const(values, "lshr")};
static constexpr uint64_t kOpShl = {c_const(values, "shl")};
static constexpr uint64_t kOpNot = {c_const(values, "not")};
static constexpr uint64_t kOpUDiv = {c_const(values, "udiv")};
static constexpr uint64_t kOpURem = {c_const(values, "urem")};
static constexpr uint64_t kOpEq = {c_const(values, "eq")};
static constexpr uint64_t kOpIsZero = {c_const(values, "iszero")};
static constexpr uint64_t kOpSelect = {c_const(values, "select")};
static constexpr uint64_t kOpAddC = {c_const(values, "addc")};
static constexpr uint64_t kOpXorC = {c_const(values, "xorc")};
static constexpr uint64_t kOpNe = {c_const(values, "ne")};
static constexpr uint64_t kOpUlt = {c_const(values, "ult")};
static constexpr uint64_t kOpUle = {c_const(values, "ule")};
static constexpr uint64_t kOpUgt = {c_const(values, "ugt")};
static constexpr uint64_t kOpUge = {c_const(values, "uge")};
static constexpr uint64_t kOpSlt = {c_const(values, "slt")};
static constexpr uint64_t kOpSle = {c_const(values, "sle")};
static constexpr uint64_t kOpSgt = {c_const(values, "sgt")};
static constexpr uint64_t kOpSge = {c_const(values, "sge")};
static constexpr uint64_t kOpAShr = {c_const(values, "ashr")};
static constexpr uint64_t kOpSDiv = {c_const(values, "sdiv")};
static constexpr uint64_t kOpSRem = {c_const(values, "srem")};

extern "C" uint64_t target_function(uint64_t encoded_op, uint64_t encoded_a, uint64_t encoded_b, uint64_t encoded_c)
{{
    uint64_t op = encoded_op ^ kEncodeOpXor;
    uint64_t a = encoded_a ^ kEncodeAXor;
    uint64_t b = encoded_b ^ kEncodeBXor;
    uint64_t c = encoded_c ^ kEncodeCXor;

    uint64_t safe_b = b | LOKI_IS_ZERO64(b);
    uint64_t sh = b & 63ull;
    uint64_t sel_mask = 0ull - (c & 1ull);

    uint64_t sign_a = a >> 63;
    uint64_t sign_b = b >> 63;
    uint64_t abs_a = (a ^ (0ull - sign_a)) + sign_a;
    uint64_t abs_b = (b ^ (0ull - sign_b)) + sign_b;
    uint64_t safe_abs_b = abs_b | LOKI_IS_ZERO64(abs_b);
    uint64_t signed_quotient = abs_a / safe_abs_b;
    uint64_t signed_remainder = abs_a % safe_abs_b;
    uint64_t quotient_sign = sign_a ^ sign_b;
    uint64_t signed_div = (signed_quotient ^ (0ull - quotient_sign)) + quotient_sign;
    uint64_t signed_rem = (signed_remainder ^ (0ull - sign_a)) + sign_a;
    uint64_t ashr_fill = (0ull - sign_a) << ((64ull - sh) & 63ull);
    ashr_fill &= 0ull - (LOKI_IS_ZERO64(sh) ^ 1ull);
    uint64_t signed_shr = (a >> sh) | ashr_fill;

    uint64_t r = 0;
    r |= LOKI_MASK_EQ(op, kOpAdd) & (a + b);
    r |= LOKI_MASK_EQ(op, kOpSub) & (a - b);
    r |= LOKI_MASK_EQ(op, kOpXor) & (a ^ b);
    r |= LOKI_MASK_EQ(op, kOpAnd) & (a & b);
    r |= LOKI_MASK_EQ(op, kOpOr) & (a | b);
    r |= LOKI_MASK_EQ(op, kOpMul) & (a * b);
    r |= LOKI_MASK_EQ(op, kOpLShr) & (a >> sh);
    r |= LOKI_MASK_EQ(op, kOpShl) & (a << sh);
    r |= LOKI_MASK_EQ(op, kOpNot) & (~a);
    r |= LOKI_MASK_EQ(op, kOpUDiv) & (a / safe_b);
    r |= LOKI_MASK_EQ(op, kOpURem) & (a % safe_b);
    r |= LOKI_MASK_EQ(op, kOpEq) & LOKI_IS_ZERO64(a ^ b);
    r |= LOKI_MASK_EQ(op, kOpIsZero) & LOKI_IS_ZERO64(a);
    r |= LOKI_MASK_EQ(op, kOpSelect) & ((a & sel_mask) | (b & ~sel_mask));

    uint64_t eq = LOKI_IS_ZERO64(a ^ b);
    uint64_t ult = LOKI_ULT64(a, b);
    uint64_t ugt = LOKI_ULT64(b, a);
    uint64_t slt = LOKI_SLT64(a, b);
    uint64_t sgt = LOKI_SLT64(b, a);

    r |= LOKI_MASK_EQ(op, kOpAddC) & (a + c);
    r |= LOKI_MASK_EQ(op, kOpXorC) & (a ^ c);
    r |= LOKI_MASK_EQ(op, kOpNe) & (eq ^ 1ull);
    r |= LOKI_MASK_EQ(op, kOpUlt) & ult;
    r |= LOKI_MASK_EQ(op, kOpUle) & (ult | eq);
    r |= LOKI_MASK_EQ(op, kOpUgt) & ugt;
    r |= LOKI_MASK_EQ(op, kOpUge) & (ugt | eq);
    r |= LOKI_MASK_EQ(op, kOpSlt) & slt;
    r |= LOKI_MASK_EQ(op, kOpSle) & (slt | eq);
    r |= LOKI_MASK_EQ(op, kOpSgt) & sgt;
    r |= LOKI_MASK_EQ(op, kOpSge) & (sgt | eq);
    r |= LOKI_MASK_EQ(op, kOpAShr) & signed_shr;
    r |= LOKI_MASK_EQ(op, kOpSDiv) & signed_div;
    r |= LOKI_MASK_EQ(op, kOpSRem) & signed_rem;
    return r;
}}

static uint64_t parse_u64(const char* text)
{{
    return std::strtoull(text, nullptr, 0);
}}

int main(int argc, char** argv)
{{
    uint64_t op = argc > 1 ? parse_u64(argv[1]) : 0;
    uint64_t a = argc > 2 ? parse_u64(argv[2]) : 1;
    uint64_t b = argc > 3 ? parse_u64(argv[3]) : 2;
    uint64_t c = argc > 4 ? parse_u64(argv[4]) : 0;

    double duration_sum = 0;
    uint64_t result = 0;
    for (int i = 0; i < 10000; ++i)
    {{
        auto t1 = std::chrono::high_resolution_clock::now();
        result = target_function(op ^ kEncodeOpXor, a ^ kEncodeAXor, b ^ kEncodeBXor, c ^ kEncodeCXor);
        auto t2 = std::chrono::high_resolution_clock::now();
        duration_sum += std::chrono::duration<double, std::micro>(t2 - t1).count();
    }}

    std::printf("Output: %llu\\nTime: %lfms\\n",
                static_cast<unsigned long long>(result),
                duration_sum / 10000);
    return 0;
}}
'''


def main() -> None:
    parser = argparse.ArgumentParser(description="Generate Loki uop dispatcher source from a stable opcode map")
    parser.add_argument("--map", required=True, type=Path, help="map path to read or create")
    parser.add_argument("--output", required=True, type=Path, help="C++ source output path")
    parser.add_argument("--seed", default=None, help="deterministic seed used only when creating a new map")
    parser.add_argument("--force-map", action="store_true", help="regenerate the map even if it exists")
    args = parser.parse_args()

    if args.force_map or not args.map.exists():
        values = generate_map(args.seed)
        write_map(args.map, values)
    else:
        values = load_map(args.map)

    missing = [key for key in ENCODING_KEYS + OPERATIONS if key not in values]
    if missing:
        raise SystemExit(f"missing uop map entries: {missing}")

    args.output.parent.mkdir(parents=True, exist_ok=True)
    content = render_cpp(values)
    if args.output.exists() and args.output.read_bytes().decode("utf-8") == content:
        return
    with args.output.open("w", encoding="utf-8", newline="\n") as output:
        output.write(content)


if __name__ == "__main__":
    main()
