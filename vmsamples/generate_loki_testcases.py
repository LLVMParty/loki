#!/usr/bin/env python3
"""Generate one Loki testcase per exported vm_test_suite function.

Loki's stock obfuscate.py virtualizes a single function named target_function.
This script emits self-contained source files where each exported sample function
is renamed to target_function.
"""

from __future__ import annotations

import re
from pathlib import Path

ROOT = Path(__file__).resolve().parent
SRC = ROOT / "vm_test_suite.cpp"
OUT = ROOT.parent / "loki" / "testcases" / "vmsamples"

FUNC_RE = re.compile(
    r"VMTEST_EXPORT\s+NOINLINE\s+uint64_t\s+(vm\d+_[A-Za-z0-9_]+)\s*\(([^)]*)\)\s*\{",
    re.MULTILINE,
)


def find_matching_brace(text: str, open_brace: int) -> int:
    depth = 0
    for i in range(open_brace, len(text)):
        if text[i] == "{":
            depth += 1
        elif text[i] == "}":
            depth -= 1
            if depth == 0:
                return i
    raise ValueError("unmatched brace")


def extract_functions(source: str) -> dict[str, tuple[str, str]]:
    functions: dict[str, tuple[str, str]] = {}
    for match in FUNC_RE.finditer(source):
        name = match.group(1)
        args = match.group(2).strip()
        open_brace = source.find("{", match.start())
        close_brace = find_matching_brace(source, open_brace)
        body = source[open_brace:close_brace + 1]
        functions[name] = (args, body)
    return functions


def arg_name(arg: str) -> str:
    arg = arg.strip()
    if arg == "void":
        return ""
    match = re.search(r"([A-Za-z_]\w*)\s*$", arg)
    if not match:
        raise ValueError(f"cannot extract argument name from {arg!r}")
    return match.group(1)


def make_value(arg: str, index: int) -> tuple[list[str], str, bool]:
    name = arg_name(arg)
    arg = arg.strip()
    if arg == "void":
        return [], "", False

    if "uint64_t*" in arg and "const" not in arg:
        return [
            f"    uint64_t {name}_storage = argc > {index} ? parse_u64(argv[{index}]) : 0x123456789abcdef0ull;",
            f"    uint64_t* {name} = &{name}_storage;",
        ], name, True
    if "uint64_t*" in arg:
        return [
            f"    uint64_t {name}_storage[] = {{0, 1, 2, 3, 5, 8, 13, 21, 34, 55, 89, 144, 233, 377, 610, 987}};",
            f"    const uint64_t* {name} = {name}_storage;",
        ], name, False
    if "uint8_t*" in arg:
        return [
            f"    uint8_t {name}_storage[] = {{0x00, 0x01, 0x10, 0x20, 0x40, 0x7f, 0x80, 0x81, 0xff, 0x55, 0xaa, 0x42, 0x99, 0x11, 0x22, 0x33}};",
            f"    const uint8_t* {name} = {name}_storage;",
        ], name, False
    if "uint32_t" in arg:
        return [f"    uint32_t {name} = static_cast<uint32_t>(argc > {index} ? parse_u64(argv[{index}]) : {index});"], name, True
    if "uint8_t" in arg:
        return [f"    uint8_t {name} = static_cast<uint8_t>(argc > {index} ? parse_u64(argv[{index}]) : {index});"], name, True
    if "uint64_t" in arg:
        return [f"    uint64_t {name} = argc > {index} ? parse_u64(argv[{index}]) : {index};"], name, True
    raise ValueError(f"unsupported argument type: {arg!r}")


def manual_vm05_body() -> str:
    # Avoid LLVM select over an i1 condition; vm_alu expects same-width values.
    return """{
    uint64_t mask = 0ull - (sel & 1ull);
    return (a & mask) | (b & ~mask);
}"""


def manual_vm21_body() -> str:
    # Avoid a helper call in target_function. lift_input only translates the
    # target function body and treats calls as unsupported instructions.
    return """{
    uint64_t x0 = x + y;
    uint64_t y0 = 0x42ull ^ 0x1122334455667788ull;
    uint64_t z0 = x0 & 0xFF00FF00FF00FF00ull;
    uint64_t t = (z0 ^ y0) + 0x55aa55aa55aa55aaull;
    uint64_t r = 0;
    if (t == 42)
        r = 1337;
    else if (t < 42)
        r = t + 7;
    else
        r = t - 7;
    return r + 1;
}"""


def main() -> None:
    source = SRC.read_text()
    functions = extract_functions(source)
    if not functions:
        raise SystemExit("no exported functions found")

    OUT.mkdir(parents=True, exist_ok=True)

    for name, (args_text, body) in functions.items():
        args = [a.strip() for a in args_text.split(",")]
        if args == ["void"]:
            args = []
        signature = ", ".join(args) if args else "void"

        extra = ""
        if name == "vm05_select":
            body = manual_vm05_body()
        elif name == "vm18_out_param":
            extra = "namespace { volatile uint64_t g_scratch = 0; }\n\n"
        elif name == "vm19_global_alias":
            extra = "namespace { volatile int g_slot0 = 0; volatile int g_slot1 = 1; }\n\n"
        elif name == "vm21_chain":
            body = manual_vm21_body()

        setup_lines: list[str] = []
        argv_index = 1
        value_names: list[str] = []
        for arg in args:
            lines, value, consumes = make_value(arg, argv_index)
            setup_lines.extend(lines)
            if value:
                value_names.append(value)
            if consumes:
                argv_index += 1

        main_call = f"target_function({', '.join(value_names)})" if value_names else "target_function()"
        setup_code = "\n".join(setup_lines) if setup_lines else "    (void)argc;\n    (void)argv;"

        content = f"""// Generated by vmsamples/generate_loki_testcases.py.
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>

{extra}extern \"C\" uint64_t target_function({signature})
{body}

static uint64_t parse_u64(const char* text)
{{
    return std::strtoull(text, nullptr, 0);
}}

int main(int argc, char** argv)
{{
{setup_code}

    double duration_sum = 0;
    uint64_t result = 0;
    for (int i = 0; i < 10000; ++i)
    {{
        auto t1 = std::chrono::high_resolution_clock::now();
        result = {main_call};
        auto t2 = std::chrono::high_resolution_clock::now();
        duration_sum += std::chrono::duration<double, std::micro>(t2 - t1).count();
    }}

    std::printf(\"Output: %llu\\nTime: %lfms\\n\",
                static_cast<unsigned long long>(result),
                duration_sum / 10000);
    return 0;
}}
"""
        (OUT / f"{name}.cpp").write_text(content)

    print(f"generated {len(functions)} testcases in {OUT}")


if __name__ == "__main__":
    main()
