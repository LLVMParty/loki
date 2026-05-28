#ifndef VMTEST_NO_INCLUDES
#include <cstddef>
#include <cstdint>
#endif
#ifndef NOINLINE
#if defined(_MSC_VER)
#define NOINLINE __declspec(noinline)
#else
#define NOINLINE __attribute__((noinline))
#endif
#endif

#ifndef VMTEST_VIRTUALIZE
#if defined(__clang__)
#define VMTEST_VIRTUALIZE __attribute__((annotate("loki_virtualize")))
#else
#define VMTEST_VIRTUALIZE
#endif
#endif

#ifndef VMTEST_EXPORT
#ifdef _WIN32
#define VMTEST_EXPORT extern "C" __declspec(dllexport)
#else
#define VMTEST_EXPORT extern "C" __attribute__((visibility("default")))
#endif
#endif

// ===========================================================================
//  Devirtualization test suite — ordered by recovery difficulty
//  Compile with: clang++ -O1 -c vm_test_suite.cpp
// ===========================================================================

// ===== TIER 1: Straight-line (single basic block, no control flow) =========

// ---------------------------------------------------------------------------
// 1. Trivial — single op, absolute baseline
// ---------------------------------------------------------------------------
VMTEST_EXPORT VMTEST_VIRTUALIZE NOINLINE uint64_t vm01_xor(uint64_t a, uint64_t b)
{
    return a ^ b;
}

// ---------------------------------------------------------------------------
// 2. Mixed ops — add, xor, and, constants in one block
// ---------------------------------------------------------------------------
VMTEST_EXPORT VMTEST_VIRTUALIZE NOINLINE uint64_t vm02_ops(uint64_t a, uint64_t b, uint64_t c)
{
    uint64_t x = a + b;
    uint64_t y = c ^ 0x1122334455667788ull;
    uint64_t z = x & 0xFF00FF00FF00FF00ull;
    return (z ^ y) + 0x55aa55aa55aa55aaull;
}

// ---------------------------------------------------------------------------
// 3. Mixed-width — 32-bit ops in 64-bit context, still straight-line
//    but tests width recovery (VMs often normalize to one register width)
// ---------------------------------------------------------------------------
VMTEST_EXPORT VMTEST_VIRTUALIZE NOINLINE uint64_t vm03_mixed_width(uint64_t a, uint64_t b)
{
    uint32_t lo_a = static_cast<uint32_t>(a);
    uint32_t lo_b = static_cast<uint32_t>(b);
    uint32_t hi_a = static_cast<uint32_t>(a >> 32);
    uint32_t hi_b = static_cast<uint32_t>(b >> 32);

    uint32_t sum_lo = lo_a + lo_b;
    uint32_t xor_hi = hi_a ^ hi_b;

    uint64_t combined = (static_cast<uint64_t>(xor_hi) << 32) | sum_lo;
    return combined + 0x0101010101010101ull;
}

// ---------------------------------------------------------------------------
// 4. Division — straight-line at -O1, but many lifters (Miasm, etc.) split
//    div/idiv into multiple BBs for the implicit zero-check
// ---------------------------------------------------------------------------
VMTEST_EXPORT VMTEST_VIRTUALIZE NOINLINE uint64_t vm04_division(uint64_t n, uint64_t m)
{
    uint64_t q = n / m;
    uint64_t r = n % m;
    return (q << 32) | r;
}

// ===== TIER 2: Simple conditionals ========================================

// ---------------------------------------------------------------------------
// 5. Branchless select — compiles to cmov, data-dependent choice
// ---------------------------------------------------------------------------
VMTEST_EXPORT VMTEST_VIRTUALIZE NOINLINE uint64_t vm05_select(uint64_t a, uint64_t b, uint64_t sel)
{
    return (sel & 1) ? a : b;
}

// ---------------------------------------------------------------------------
// 6. Single branch — one jcc, forced via loop form to prevent cmov
// ---------------------------------------------------------------------------
VMTEST_EXPORT VMTEST_VIRTUALIZE NOINLINE uint64_t vm06_single_jcc(uint64_t a, uint64_t b)
{
    for (; a == 1337;)
    {
        asm volatile("" ::: "memory");
        return b;
    }
    return a;
}

// ---------------------------------------------------------------------------
// 7. Multiple branches — comparisons, multiple exits
// ---------------------------------------------------------------------------
VMTEST_EXPORT VMTEST_VIRTUALIZE NOINLINE uint64_t vm07_ifelse(uint64_t x)
{
    if (x == 42)
        return 1337;
    if (x < 42)
        return x + 7;
    return x - 7;
}

// ===== TIER 3: Simple loops ===============================================

// ---------------------------------------------------------------------------
// 8. Constant-trip-count loop — can the tool collapse it entirely?
// ---------------------------------------------------------------------------
VMTEST_EXPORT VMTEST_VIRTUALIZE NOINLINE uint64_t vm08_const_loop(void)
{
    uint64_t acc = 0;
#pragma clang loop unroll(disable)
    for (uint64_t i = 0; i < 8; ++i)
    {
        acc += (i ^ 3ull) + 1ull;
        asm volatile("" : "+r"(acc) :: "memory");
    }
    return acc;
}

// ---------------------------------------------------------------------------
// 9. Dynamic-trip-count loop — cannot be statically resolved
// ---------------------------------------------------------------------------
VMTEST_EXPORT VMTEST_VIRTUALIZE NOINLINE uint64_t vm09_counted_sum(uint64_t count, uint64_t step)
{
    uint64_t acc = 0;
#pragma clang loop unroll(disable)
    while (count != 0)
    {
        acc += step;
        asm volatile("" : "+r"(acc) :: "memory");
        count -= 1;
    }
    return acc;
}

// ---------------------------------------------------------------------------
// 10. do-while — post-condition loop, different CFG shape (back-edge always
//     taken at least once), trip count depends on low-byte alignment
// ---------------------------------------------------------------------------
VMTEST_EXPORT VMTEST_VIRTUALIZE NOINLINE uint64_t vm10_do_while(uint64_t n)
{
    uint64_t acc = 0;
    do
    {
        asm volatile("" : "+r"(acc), "+r"(n) :: "memory");
        acc += n;
        n++;
    } while ((n & 0xFF) != 0);
    return acc;
}

// ---------------------------------------------------------------------------
// 11. Non-linear stride — i *= 2, log2 iterations, non-affine induction
//     variable (most loop analysis assumes i += constant)
// ---------------------------------------------------------------------------
VMTEST_EXPORT VMTEST_VIRTUALIZE NOINLINE uint64_t vm11_exp_stride(uint64_t n)
{
    uint64_t acc = 1;
#pragma clang loop unroll(disable)
    for (uint64_t i = 1; i <= n; i *= 2)
    {
        asm volatile("" : "+r"(acc) :: "memory");
        acc *= i;
    }
    return acc;
}

// ===== TIER 4: Loop + control flow ========================================

// ---------------------------------------------------------------------------
// 12. Loop with branch — mixed control flow inside loop body
// ---------------------------------------------------------------------------
VMTEST_EXPORT VMTEST_VIRTUALIZE NOINLINE uint64_t vm12_loop_branch(const uint64_t* arr, uint64_t n)
{
    uint64_t even_sum = 0, odd_sum = 0;
#pragma clang loop unroll(disable)
    for (uint64_t i = 0; i < n; ++i)
    {
        if (arr[i] & 1) {
            asm volatile("" ::: "memory");
            odd_sum += arr[i];
        }
        else {
            even_sum += arr[i];
        }
    }
    return even_sum ^ odd_sum;
}

// ===== TIER 5: Switch / dispatch recovery =================================

// ---------------------------------------------------------------------------
// 13. Small switch — 4 cases, basic jump table
// ---------------------------------------------------------------------------
VMTEST_EXPORT VMTEST_VIRTUALIZE NOINLINE uint64_t vm13_switch4(uint32_t opcode, uint64_t a, uint64_t b)
{
    switch (opcode & 3u)
    {
    case 0:  return a + b;
    case 1:  return a ^ b;
    case 2:  return (a | b) + 0x100ull;
    default: return (a * 3ull) - b;
    }
}

// ---------------------------------------------------------------------------
// 14. Large switch — 12 cases, realistic VM dispatcher size
// ---------------------------------------------------------------------------
VMTEST_EXPORT VMTEST_VIRTUALIZE NOINLINE uint64_t vm14_switch12(uint64_t op, uint64_t a, uint64_t b)
{
    switch (op)
    {
    case 0:  return a + b;
    case 1:  return a - b;
    case 2:  return a ^ b;
    case 3:  return a | b;
    case 4:  return a & b;
    case 5:  return a * b;
    case 6:  return ~a ^ b;
    case 7:  return a + (b << 2);
    case 8:  return (a >> 1) | (b << 63);
    case 9:  return a ^ (b * 7);
    case 10: return (a | 0xFF) + b;
    case 11: return (a & b) + (a ^ b);
    default: return a;
    }
}

// ---------------------------------------------------------------------------
// 15. Large switch with shared handlers — 48 contiguous cases mapping to
//     6 handlers (8 cases each). Clang emits a flat table with repeated
//     targets. Tests: deduplicating shared handlers, recognizing that
//     multiple table entries resolve to the same code.
//     NOTE: the two-level byte_table -> jump_table pattern is a virtualizer
//     artifact, not something Clang emits; test that against actual VM output.
// ---------------------------------------------------------------------------
VMTEST_EXPORT VMTEST_VIRTUALIZE NOINLINE uint64_t vm15_switch_shared(uint64_t n, uint64_t a)
{
    switch (n)
    {
    case 0:  case 1:  case 2:  case 3:
    case 4:  case 5:  case 6:  case 7:
        return a + 10;
    case 8:  case 9:  case 10: case 11:
    case 12: case 13: case 14: case 15:
        return a * 3;
    case 16: case 17: case 18: case 19:
    case 20: case 21: case 22: case 23:
        return a ^ 0xFFull;
    case 24: case 25: case 26: case 27:
    case 28: case 29: case 30: case 31:
        return a >> 2;
    case 32: case 33: case 34: case 35:
    case 36: case 37: case 38: case 39:
        return a + (a << 1);
    case 40: case 41: case 42: case 43:
    case 44: case 45: case 46: case 47:
        return (a & 0xF0F0F0F0ull) | 1;
    default:
        return a;
    }
}

// ===== TIER 6: Complex memory patterns ====================================

// ---------------------------------------------------------------------------
// 16. Data-dependent accumulator — XOR feedback loop, memory reads
// ---------------------------------------------------------------------------
VMTEST_EXPORT VMTEST_VIRTUALIZE NOINLINE uint64_t vm16_xor_checksum(const uint8_t* data, uint64_t size, uint8_t key)
{
    uint64_t acc = 0;
#pragma clang loop unroll(disable)
    for (uint64_t i = 0; i < size; ++i)
    {
        asm volatile("" ::: "memory");
        acc += static_cast<uint64_t>(data[i] ^ key);
        acc ^= (acc << 7);
        acc += 0x13579BDF2468ACE0ull;
    }
    return acc;
}

// ---------------------------------------------------------------------------
// 17. Pointer chasing / table walk — indirect memory, common in obfuscation
// ---------------------------------------------------------------------------
VMTEST_EXPORT VMTEST_VIRTUALIZE NOINLINE uint64_t vm17_table_walk(const uint64_t* table, uint64_t idx, uint64_t depth)
{
    uint64_t cur = idx;
#pragma clang loop unroll(disable)
    for (uint64_t d = 0; d < depth; ++d)
    {
        asm volatile("" ::: "memory");
        cur = table[cur & 0xF];
    }
    return cur;
}

// ---------------------------------------------------------------------------
// 18. Pointer out-parameter — side effect through memory write + return,
//     tests whether the tool tracks aliased stores
// ---------------------------------------------------------------------------
namespace { volatile uint64_t g_scratch = 0; }

VMTEST_EXPORT VMTEST_VIRTUALIZE NOINLINE uint64_t vm18_out_param(uint64_t* x)
{
    uint64_t old = *x;
    *x = old * 3 + 7;
    g_scratch = old;  // second memory side effect
    return old ^ *x;
}

// ---------------------------------------------------------------------------
// 19. Global mutation + pointer aliasing — modifies statics through pointers,
//     then indexes array with result. Nasty for symbolic execution.
// ---------------------------------------------------------------------------
namespace {
    volatile int g_slot0 = 0;
    volatile int g_slot1 = 1;
}

VMTEST_EXPORT VMTEST_VIRTUALIZE NOINLINE uint64_t vm19_global_alias(uint64_t n)
{
    volatile int* ptr;
    if ((n & 1) == 0)
    {
        ptr = &g_slot0;
        *ptr = 1;
    }
    else
    {
        ptr = &g_slot1;
        *ptr = 0;
    }
    uint64_t table[] = { 0xAAAA, 0xBBBB };
    return table[*ptr] + n;
}

// ===== TIER 7: Structural complexity ======================================

// ---------------------------------------------------------------------------
// 20. Nested loops — multiple loop headers to recover
// ---------------------------------------------------------------------------
VMTEST_EXPORT VMTEST_VIRTUALIZE NOINLINE uint64_t vm20_nested_loop(uint64_t rows, uint64_t cols)
{
    uint64_t acc = 0;
#pragma clang loop unroll(disable)
    for (uint64_t r = 0; r < rows; ++r)
    {
        asm volatile("" ::: "memory");
#pragma clang loop unroll(disable)
        for (uint64_t c = 0; c < cols; ++c)
        {
            asm volatile("" ::: "memory");
            acc += (r * cols + c) ^ 0xDEADull;
        }
    }
    return acc;
}

// ---------------------------------------------------------------------------
// 21. Cross-function — handler composition / call recovery
// ---------------------------------------------------------------------------
VMTEST_EXPORT VMTEST_VIRTUALIZE NOINLINE uint64_t vm21_chain(uint64_t x, uint64_t y)
{
    uint64_t t = vm02_ops(x, y, 0x42);
    return vm07_ifelse(t) + 1;
}

// ===== TIER 8: Obfuscation-specific patterns ==============================

// ---------------------------------------------------------------------------
// 22. Opaque predicate + MBA — expression that always evaluates true but
//     looks non-trivial; mixed-boolean-arithmetic style
// ---------------------------------------------------------------------------
VMTEST_EXPORT VMTEST_VIRTUALIZE NOINLINE uint64_t vm22_opaque_mba(uint64_t x, uint64_t y)
{
    // (x | y) - (x & y) == (x ^ y)  always, but the volatile prevents
    // the compiler from proving it at compile time
    volatile uint64_t vx = x;
    volatile uint64_t vy = y;
    uint64_t lhs = (vx | vy) + (~(vx & vy)) + 1ull;  // (x|y) - (x&y)
    uint64_t rhs = vx ^ vy;

    // opaque predicate: lhs == rhs is always true
    if (lhs == rhs)
        return (x * 31ull) ^ (y + 0xCAFEBABEull);
    else
        return 0xDEADDEADDEADDEADull;  // dead path
}

// ---------------------------------------------------------------------------
// 23. State-machine loop — loop body's path depends on state modified by
//     the previous iteration; defeats simple symbolic execution scaling
// ---------------------------------------------------------------------------
VMTEST_EXPORT VMTEST_VIRTUALIZE NOINLINE uint64_t vm23_state_machine(const uint8_t* input, uint64_t len)
{
    uint64_t state = 0;
    uint64_t acc   = 0;

#pragma clang loop unroll(disable)
    for (uint64_t i = 0; i < len; ++i)
    {
        asm volatile("" ::: "memory");
        uint8_t b = input[i];

        switch (state & 3ull)
        {
        case 0:
            acc += b;
            state = (b > 0x80) ? 1 : 2;
            break;
        case 1:
            acc ^= static_cast<uint64_t>(b) << 3;
            state = (acc & 1) ? 3 : 0;
            break;
        case 2:
            acc -= b;
            state = (b & 0x10) ? 0 : 3;
            break;
        default:
            acc = (acc << 1) | (b & 1);
            state = (acc > 0x1000) ? 1 : 2;
            break;
        }
    }
    return acc;
}
