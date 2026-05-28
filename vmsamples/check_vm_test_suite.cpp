#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
using HMODULE = void*;
static const char* g_last_load_error = "unknown error";
static HMODULE LoadLibraryA(const char* path)
{
    dlerror();
    HMODULE handle = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!handle)
        g_last_load_error = dlerror();
    return handle;
}
static void* GetProcAddress(HMODULE module, const char* name)
{
    return dlsym(module, name);
}
static int FreeLibrary(HMODULE module)
{
    return dlclose(module);
}
#endif

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

// ===========================================================================
//  Reference implementations — include the actual source with neutralized
//  export/noinline macros. The asm volatile barriers are semantic NOPs so
//  the reference computes identical results.
// ===========================================================================

#define VMTEST_EXPORT static
#define NOINLINE
#define VMTEST_NO_INCLUDES
#include <cstddef>
#include <cstdint>
namespace ref {
#include "vm_test_suite.cpp"
} // namespace ref

// ===========================================================================
//  Test harness
// ===========================================================================

namespace
{

const char* base_name(const char* path)
{
    const char* slash = std::strrchr(path, '\\');
    const char* fwd   = std::strrchr(path, '/');
    const char* sep   = slash > fwd ? slash : fwd;
    return sep ? sep + 1 : path;
}

struct Counters
{
    unsigned started = 0;
    unsigned passed  = 0;
    unsigned failed  = 0;
    unsigned skipped = 0;

    void record(const char* tag, uint64_t got, uint64_t expected)
    {
        if (got == expected) { ++passed; return; }
        std::printf("  FAIL  %-32s got=0x%016llx expected=0x%016llx\n",
                    tag,
                    static_cast<unsigned long long>(got),
                    static_cast<unsigned long long>(expected));
        ++failed;
    }

    void skip(const char* name)
    {
        std::printf("  SKIP  %s (not exported)\n", name);
        ++skipped;
    }

    template <typename GotFn, typename ExpectedFn>
    void record_logged(const char* tag, int line, GotFn got_fn, ExpectedFn expected_fn)
    {
        ++started;
        std::printf("  RUN   #%04u %-32s line=%d\n", started, tag, line);
        std::fflush(stdout);

        uint64_t got = got_fn();
        uint64_t expected = expected_fn();
        record(tag, got, expected);
    }
};

#define CHECK_RECORD(counter, tag, got, expected) \
    (counter).record_logged((tag), __LINE__, \
                            [&]() -> uint64_t { return static_cast<uint64_t>(got); }, \
                            [&]() -> uint64_t { return static_cast<uint64_t>(expected); })

template <typename T>
T try_load(HMODULE dll, const char* name)
{
    return reinterpret_cast<T>(GetProcAddress(dll, name));
}

constexpr uint64_t U64MAX = 0xFFFFFFFFFFFFFFFFull;
constexpr uint64_t HALF   = 0x8000000000000000ull;
constexpr uint64_t ALT_A  = 0xAAAAAAAAAAAAAAAAull;
constexpr uint64_t ALT_5  = 0x5555555555555555ull;
constexpr uint64_t HI32   = 0xFFFFFFFF00000000ull;
constexpr uint64_t LO32   = 0x00000000FFFFFFFFull;

} // namespace

// ===========================================================================
//  Per-function test drivers
//  Branch coverage notes in comments; [B:N/M] = N of M source branches hit
// ===========================================================================

// vm01: no branches
static void test_vm01(HMODULE dll, Counters& c)
{
    using fn_t = uint64_t(*)(uint64_t, uint64_t);
    auto fn = try_load<fn_t>(dll, "vm01_xor");
    if (!fn) { c.skip("vm01_xor"); return; }

    struct { uint64_t a, b; } cases[] = {
        {0, 0}, {0, U64MAX}, {U64MAX, U64MAX}, {1, 1},
        {ALT_A, ALT_5}, {HI32, LO32}, {0x123456789ABCDEF0ull, 0x0FEDCBA987654321ull},
        {42, 42}, {1337, 0}, {HALF, HALF},
    };
    for (auto& t : cases)
        CHECK_RECORD(c, "vm01_xor", fn(t.a, t.b), ref::vm01_xor(t.a, t.b));
}

// vm02: no branches
static void test_vm02(HMODULE dll, Counters& c)
{
    using fn_t = uint64_t(*)(uint64_t, uint64_t, uint64_t);
    auto fn = try_load<fn_t>(dll, "vm02_ops");
    if (!fn) { c.skip("vm02_ops"); return; }

    struct { uint64_t a, b, cc; } cases[] = {
        {0, 0, 0}, {1, 2, 3}, {U64MAX, 1, 0}, {U64MAX, U64MAX, U64MAX},
        {0x123456789ABCDEF0ull, 0x0FEDCBA987654321ull, ALT_5},
        {0, 0, 0x1122334455667788ull}, {HI32, LO32, HALF},
    };
    for (auto& t : cases)
        CHECK_RECORD(c, "vm02_ops", fn(t.a, t.b, t.cc), ref::vm02_ops(t.a, t.b, t.cc));
}

// vm03: no branches
static void test_vm03(HMODULE dll, Counters& c)
{
    using fn_t = uint64_t(*)(uint64_t, uint64_t);
    auto fn = try_load<fn_t>(dll, "vm03_mixed_width");
    if (!fn) { c.skip("vm03_mixed_width"); return; }

    struct { uint64_t a, b; } cases[] = {
        {0, 0}, {1, 2}, {LO32, LO32}, {HI32, HI32},
        {U64MAX, 1}, {0x0000000100000000ull, 0x0000000200000000ull},
        {0x00000001FFFFFFFFull, 0x0000000200000001ull}, {ALT_A, ALT_5},
    };
    for (auto& t : cases)
        CHECK_RECORD(c, "vm03_mixed_width", fn(t.a, t.b), ref::vm03_mixed_width(t.a, t.b));
}

// vm04: [B:2/2] sel&1 true, sel&1 false
static void test_vm05(HMODULE dll, Counters& c)
{
    using fn_t = uint64_t(*)(uint64_t, uint64_t, uint64_t);
    auto fn = try_load<fn_t>(dll, "vm05_select");
    if (!fn) { c.skip("vm05_select"); return; }

    struct { uint64_t a, b, sel; } cases[] = {
        {100, 200, 0}, {100, 200, 1}, {100, 200, 2}, {100, 200, 3},
        {U64MAX, 0, 1}, {0, U64MAX, 0},
        {42, 1337, U64MAX}, {42, 1337, 0xFFFFFFFFFFFFFFFEull},
    };
    for (auto& t : cases)
        CHECK_RECORD(c, "vm05_select", fn(t.a, t.b, t.sel), ref::vm05_select(t.a, t.b, t.sel));
}

// vm05: [B:2/2] a==1337, a!=1337
static void test_vm06(HMODULE dll, Counters& c)
{
    using fn_t = uint64_t(*)(uint64_t, uint64_t);
    auto fn = try_load<fn_t>(dll, "vm06_single_jcc");
    if (!fn) { c.skip("vm06_single_jcc"); return; }

    struct { uint64_t a, b; } cases[] = {
        {1337, 0xDEAD}, {1337, 0}, {1337, U64MAX},  // a==1337 -> b
        {0, 99}, {1, 99}, {1336, 99}, {1338, 99}, {U64MAX, 99},  // a!=1337 -> a
    };
    for (auto& t : cases)
        CHECK_RECORD(c, "vm06_single_jcc", fn(t.a, t.b), ref::vm06_single_jcc(t.a, t.b));
}

// vm06: [B:3/3] x==42, x<42, x>42
static void test_vm07(HMODULE dll, Counters& c)
{
    using fn_t = uint64_t(*)(uint64_t);
    auto fn = try_load<fn_t>(dll, "vm07_ifelse");
    if (!fn) { c.skip("vm07_ifelse"); return; }

    uint64_t cases[] = {
        0, 1, 7, 34, 35, 41,   // < 42
        42,                      // == 42
        43, 49, 100, 1000, U64MAX,  // > 42
    };
    for (uint64_t x : cases)
        CHECK_RECORD(c, "vm07_ifelse", fn(x), ref::vm07_ifelse(x));
}

// vm07: no source branches (compiler may add 32/64-bit div split at -O2)
// test both small and large values to cover the compiler-generated branch
static void test_vm04(HMODULE dll, Counters& c)
{
    using fn_t = uint64_t(*)(uint64_t, uint64_t);
    auto fn = try_load<fn_t>(dll, "vm04_division");
    if (!fn) { c.skip("vm04_division"); return; }

    struct { uint64_t n, m; } cases[] = {
        // small (both fit in 32 bits — triggers div esi at -O2)
        {0, 1}, {1, 1}, {7, 3}, {100, 7}, {100, 100}, {100, 101},
        // large (need div rsi path)
        {U64MAX, 1}, {U64MAX, 2}, {U64MAX, U64MAX},
        {0x123456789ABCDEF0ull, 17}, {HALF, 3},
        // one large, one small (still needs 64-bit path)
        {0x100000000ull, 1}, {1, 0x100000000ull},
        {1000000007ull, 1000000007ull}, {999999999999999989ull, 7},
    };
    for (auto& t : cases)
        CHECK_RECORD(c, "vm04_division", fn(t.n, t.m), ref::vm04_division(t.n, t.m));
}

// vm08: loop always runs 8 iterations, no conditional branches in body
static void test_vm08(HMODULE dll, Counters& c)
{
    using fn_t = uint64_t(*)();
    auto fn = try_load<fn_t>(dll, "vm08_const_loop");
    if (!fn) { c.skip("vm08_const_loop"); return; }
    CHECK_RECORD(c, "vm08_const_loop", fn(), ref::vm08_const_loop());
}

// vm09: [B:2/2] count==0 (skip), count>0 (loop)
static void test_vm09(HMODULE dll, Counters& c)
{
    using fn_t = uint64_t(*)(uint64_t, uint64_t);
    auto fn = try_load<fn_t>(dll, "vm09_counted_sum");
    if (!fn) { c.skip("vm09_counted_sum"); return; }

    struct { uint64_t count, step; } cases[] = {
        {0, 0}, {0, 5}, {0, U64MAX},
        {1, 0}, {1, 1}, {1, U64MAX},
        {2, 1}, {5, 3}, {10, 7}, {100, 1}, {256, 1},
        {17, 0x1000000000000001ull}, {3, U64MAX},
    };
    for (auto& t : cases)
        CHECK_RECORD(c, "vm09_counted_sum", fn(t.count, t.step), ref::vm09_counted_sum(t.count, t.step));
}

// vm10: do-while always enters. [B:2/2] exit true (n&0xFF==0), continue (n&0xFF!=0)
static void test_vm10(HMODULE dll, Counters& c)
{
    using fn_t = uint64_t(*)(uint64_t);
    auto fn = try_load<fn_t>(dll, "vm10_do_while");
    if (!fn) { c.skip("vm10_do_while"); return; }

    uint64_t cases[] = {
        0, 1, 2, 0x7F, 0x80, 0xFE,
        0xFF,    // 1 iteration: 0xFF+1=0x100, 0x100&0xFF=0
        0x100, 0x1FF, 0x200,
        0xFFFFFFFFFFFFFF00ull, 0xFFFFFFFFFFFFFFFFull,
    };
    for (uint64_t n : cases)
        CHECK_RECORD(c, "vm10_do_while", fn(n), ref::vm10_do_while(n));
}

// vm11: [B:2/2] n==0 (skip), n>0 (loop)
static void test_vm11(HMODULE dll, Counters& c)
{
    using fn_t = uint64_t(*)(uint64_t);
    auto fn = try_load<fn_t>(dll, "vm11_exp_stride");
    if (!fn) { c.skip("vm11_exp_stride"); return; }

    uint64_t cases[] = {
        0, 1, 2, 3, 4, 7, 8, 15, 16, 31, 32, 63, 64, 128, 1024,
        0x4000000000000000ull, HALF - 1,
    };
    for (uint64_t n : cases)
        CHECK_RECORD(c, "vm11_exp_stride", fn(n), ref::vm11_exp_stride(n));
}

// vm12: [B:3/3] n==0 (skip), arr[i]&1 (odd path), !(arr[i]&1) (even path)
static void test_vm12(HMODULE dll, Counters& c)
{
    using fn_t = uint64_t(*)(const uint64_t*, uint64_t);
    auto fn = try_load<fn_t>(dll, "vm12_loop_branch");
    if (!fn) { c.skip("vm12_loop_branch"); return; }

    CHECK_RECORD(c, "vm12[empty]", fn(nullptr, 0), ref::vm12_loop_branch(nullptr, 0));

    uint64_t all_even[] = {2, 4, 6, 8, 10};
    CHECK_RECORD(c, "vm12[even]", fn(all_even, 5), ref::vm12_loop_branch(all_even, 5));

    uint64_t all_odd[] = {1, 3, 5, 7, 9};
    CHECK_RECORD(c, "vm12[odd]", fn(all_odd, 5), ref::vm12_loop_branch(all_odd, 5));

    uint64_t mixed[] = {1, 2, 3, 4, 5, 6, 7, 8};
    CHECK_RECORD(c, "vm12[mixed]", fn(mixed, 8), ref::vm12_loop_branch(mixed, 8));

    uint64_t one_even[] = {42};
    CHECK_RECORD(c, "vm12[1even]", fn(one_even, 1), ref::vm12_loop_branch(one_even, 1));

    uint64_t one_odd[] = {43};
    CHECK_RECORD(c, "vm12[1odd]", fn(one_odd, 1), ref::vm12_loop_branch(one_odd, 1));

    uint64_t large[] = {U64MAX, U64MAX - 1, HALF, HALF + 1};
    CHECK_RECORD(c, "vm12[large]", fn(large, 4), ref::vm12_loop_branch(large, 4));
}

// vm13: [B:4/4] all 4 switch cases (mask guarantees range)
static void test_vm13(HMODULE dll, Counters& c)
{
    using fn_t = uint64_t(*)(uint32_t, uint64_t, uint64_t);
    auto fn = try_load<fn_t>(dll, "vm13_switch4");
    if (!fn) { c.skip("vm13_switch4"); return; }

    struct { uint32_t op; uint64_t a, b; } cases[] = {
        {0, 10, 20}, {1, 10, 20}, {2, 10, 20}, {3, 10, 20},
        {4, 10, 20}, {5, 10, 20}, {6, 10, 20}, {7, 10, 20},
        {0x100, 10, 20}, {0xFFFFFFFF, 10, 20},
        {0, U64MAX, 1}, {1, ALT_A, ALT_5}, {2, 0, 0}, {3, 0, U64MAX},
        {0, 0, 0}, {1, 0, 0}, {2, U64MAX, U64MAX}, {3, 1, 1},
    };
    for (auto& t : cases)
        CHECK_RECORD(c, "vm13_switch4", fn(t.op, t.a, t.b), ref::vm13_switch4(t.op, t.a, t.b));
}

// vm14: [B:13/13] cases 0-11 + default
static void test_vm14(HMODULE dll, Counters& c)
{
    using fn_t = uint64_t(*)(uint64_t, uint64_t, uint64_t);
    auto fn = try_load<fn_t>(dll, "vm14_switch12");
    if (!fn) { c.skip("vm14_switch12"); return; }

    uint64_t a_vals[] = {0, 1, 42, ALT_A, U64MAX, HALF};
    uint64_t b_vals[] = {0, 1, 42, ALT_5, U64MAX, HALF};

    for (uint64_t op = 0; op <= 12; ++op)
        for (uint64_t a : a_vals)
            for (uint64_t b : b_vals)
                CHECK_RECORD(c, "vm14_switch12", fn(op, a, b), ref::vm14_switch12(op, a, b));

    CHECK_RECORD(c, "vm14[def100]", fn(100, 42, 0), ref::vm14_switch12(100, 42, 0));
    CHECK_RECORD(c, "vm14[defmax]", fn(U64MAX, 42, 0), ref::vm14_switch12(U64MAX, 42, 0));
}

// vm15: [B:7/7] 6 handler groups + default
static void test_vm15(HMODULE dll, Counters& c)
{
    using fn_t = uint64_t(*)(uint64_t, uint64_t);
    auto fn = try_load<fn_t>(dll, "vm15_switch_shared");
    if (!fn) { c.skip("vm15_switch_shared"); return; }

    uint64_t a_vals[] = {0, 1, 0xFF, 0xF0F0F0F0ull, U64MAX, HALF, 42};

    // boundaries and midpoints of every group
    uint64_t n_vals[] = {
        0, 3, 7,  8, 11, 15,  16, 20, 23,  24, 28, 31,
        32, 35, 39,  40, 44, 47,  48, 100, U64MAX,
    };
    for (uint64_t n : n_vals)
        for (uint64_t a : a_vals)
            CHECK_RECORD(c, "vm15_switch_shared", fn(n, a), ref::vm15_switch_shared(n, a));
}

// vm16: [B:2/2] size==0 (skip), size>0 (loop)
static void test_vm16(HMODULE dll, Counters& c)
{
    using fn_t = uint64_t(*)(const uint8_t*, uint64_t, uint8_t);
    auto fn = try_load<fn_t>(dll, "vm16_xor_checksum");
    if (!fn) { c.skip("vm16_xor_checksum"); return; }

    uint8_t keys[] = {0x00, 0x5A, 0x80, 0xFF};

    for (uint8_t key : keys)
        CHECK_RECORD(c, "vm16[empty]", fn(nullptr, 0, key), ref::vm16_xor_checksum(nullptr, 0, key));

    uint8_t singles[] = {0x00, 0x01, 0x7F, 0x80, 0xFE, 0xFF};
    for (uint8_t b : singles)
        for (uint8_t key : keys)
            CHECK_RECORD(c, "vm16[1byte]", fn(&b, 1, key), ref::vm16_xor_checksum(&b, 1, key));

    uint8_t ascending[]  = {0, 1, 2, 3, 4, 5, 6, 7};
    uint8_t descending[] = {7, 6, 5, 4, 3, 2, 1, 0};
    uint8_t all_ff[]     = {0xFF, 0xFF, 0xFF, 0xFF};
    uint8_t all_00[]     = {0x00, 0x00, 0x00, 0x00};
    uint8_t mixed[]      = {0xFF, 0x00, 0x80, 0x7F, 0x42, 0x99, 0x01, 0xFE};

    struct { const uint8_t* data; uint64_t len; } bufs[] = {
        {ascending, 8}, {descending, 8}, {all_ff, 4}, {all_00, 4}, {mixed, 8},
    };
    for (auto& buf : bufs)
        for (uint8_t key : keys)
            CHECK_RECORD(c, "vm16[multi]", fn(buf.data, buf.len, key),
                     ref::vm16_xor_checksum(buf.data, buf.len, key));
}

// vm17: [B:2/2] depth==0 (skip), depth>0 (loop)
static void test_vm17(HMODULE dll, Counters& c)
{
    using fn_t = uint64_t(*)(const uint64_t*, uint64_t, uint64_t);
    auto fn = try_load<fn_t>(dll, "vm17_table_walk");
    if (!fn) { c.skip("vm17_table_walk"); return; }

    uint64_t table[16] = {5, 3, 14, 7, 0, 11, 2, 9, 12, 1, 6, 15, 8, 4, 10, 13};

    struct { uint64_t idx, depth; } cases[] = {
        {0, 0}, {0, 1}, {0, 2}, {0, 3}, {5, 1}, {15, 1},
        {0, 10}, {7, 5}, {0xFF, 1}, {0x1234, 3},
    };
    for (auto& t : cases)
        CHECK_RECORD(c, "vm17_table_walk", fn(table, t.idx, t.depth),
                 ref::vm17_table_walk(table, t.idx, t.depth));
}

// vm18: no source branches, but check both return value AND *x side effect
static void test_vm18(HMODULE dll, Counters& c)
{
    using fn_t = uint64_t(*)(uint64_t*);
    auto fn = try_load<fn_t>(dll, "vm18_out_param");
    if (!fn) { c.skip("vm18_out_param"); return; }

    uint64_t x_vals[] = {0, 1, 2, 7, 42, 100, U64MAX, HALF, 0xDEADBEEFull};
    for (uint64_t xv : x_vals)
    {
        uint64_t x_dll = xv, x_ref = xv;
        uint64_t ret_dll = fn(&x_dll);
        uint64_t ret_ref = ref::vm18_out_param(&x_ref);
        CHECK_RECORD(c, "vm18[ret]", ret_dll, ret_ref);
        CHECK_RECORD(c, "vm18[*x]", x_dll, x_ref);
    }
}

// vm19: [B:2/2] even n, odd n
static void test_vm19(HMODULE dll, Counters& c)
{
    using fn_t = uint64_t(*)(uint64_t);
    auto fn = try_load<fn_t>(dll, "vm19_global_alias");
    if (!fn) { c.skip("vm19_global_alias"); return; }

    uint64_t cases[] = {
        0, 1, 2, 3, 4, 5, 42, 43, 100, 101,
        U64MAX - 1, U64MAX, HALF, HALF + 1,
    };
    for (uint64_t n : cases)
        CHECK_RECORD(c, "vm19_global_alias", fn(n), ref::vm19_global_alias(n));
}

// vm20: [B:3/3] rows==0, cols==0, both>0
static void test_vm20(HMODULE dll, Counters& c)
{
    using fn_t = uint64_t(*)(uint64_t, uint64_t);
    auto fn = try_load<fn_t>(dll, "vm20_nested_loop");
    if (!fn) { c.skip("vm20_nested_loop"); return; }

    struct { uint64_t rows, cols; } cases[] = {
        {0, 0}, {0, 5}, {5, 0},
        {1, 1}, {1, 5}, {5, 1},
        {2, 2}, {3, 4}, {4, 3}, {8, 8},
        {1, 100}, {100, 1},
    };
    for (auto& t : cases)
        CHECK_RECORD(c, "vm20_nested_loop", fn(t.rows, t.cols), ref::vm20_nested_loop(t.rows, t.cols));
}

// vm21: no branches in vm21 itself (two calls). Exercises vm02+vm06 branches
// indirectly; those are tested directly above.
static void test_vm21(HMODULE dll, Counters& c)
{
    using fn_t = uint64_t(*)(uint64_t, uint64_t);
    auto fn = try_load<fn_t>(dll, "vm21_chain");
    if (!fn) { c.skip("vm21_chain"); return; }

    struct { uint64_t x, y; } cases[] = {
        {0, 0}, {1, 2}, {42, 0}, {0, 42},
        {U64MAX, 0}, {0, U64MAX}, {U64MAX, U64MAX},
        {ALT_A, ALT_5}, {HI32, LO32}, {HALF, 1},
        {100, 200}, {1337, 7331},
    };
    for (auto& t : cases)
        CHECK_RECORD(c, "vm21_chain", fn(t.x, t.y), ref::vm21_chain(t.x, t.y));
}

// vm22: [B:1/2] true branch always taken. Dead branch (else) is unreachable
// by design. If devirt breaks the opaque predicate, the dead branch would
// produce 0xDEADDEADDEADDEAD — the checker will catch that.
static void test_vm22(HMODULE dll, Counters& c)
{
    using fn_t = uint64_t(*)(uint64_t, uint64_t);
    auto fn = try_load<fn_t>(dll, "vm22_opaque_mba");
    if (!fn) { c.skip("vm22_opaque_mba"); return; }

    struct { uint64_t x, y; } cases[] = {
        {0, 0}, {0, 1}, {1, 0}, {1, 1},
        {U64MAX, 0}, {0, U64MAX}, {U64MAX, U64MAX},
        {ALT_A, ALT_5}, {HI32, LO32}, {HALF, HALF},
        {42, 1337}, {0xDEADBEEFull, 0xCAFEBABEull},
    };
    for (auto& t : cases)
        CHECK_RECORD(c, "vm22_opaque_mba", fn(t.x, t.y), ref::vm22_opaque_mba(t.x, t.y));
}

// vm23: [B:8/8] all 4 states x 2 conditional transitions each
//   state 0: b>0x80 -> s1, b<=0x80 -> s2
//   state 1: acc&1  -> s3, !(acc&1) -> s0
//   state 2: b&0x10 -> s0, !(b&0x10) -> s3
//   state 3: acc>0x1000 -> s1, acc<=0x1000 -> s2
static void test_vm23(HMODULE dll, Counters& c)
{
    using fn_t = uint64_t(*)(const uint8_t*, uint64_t);
    auto fn = try_load<fn_t>(dll, "vm23_state_machine");
    if (!fn) { c.skip("vm23_state_machine"); return; }

    // empty
    CHECK_RECORD(c, "vm23[empty]", fn(nullptr, 0), ref::vm23_state_machine(nullptr, 0));

    // s0 -> s2 (b<=0x80), s0 -> s1 (b>0x80)
    uint8_t lo = 0x10, hi = 0x90;
    CHECK_RECORD(c, "vm23[s0->s2]", fn(&lo, 1), ref::vm23_state_machine(&lo, 1));
    CHECK_RECORD(c, "vm23[s0->s1]", fn(&hi, 1), ref::vm23_state_machine(&hi, 1));

    // s1 -> s3 (acc odd): {0x81, 0x01} -> s0:acc=0x81,s=1 -> s1:acc=0x89(odd),s=3
    uint8_t s1_to_s3[] = {0x81, 0x01};
    CHECK_RECORD(c, "vm23[s1->s3]", fn(s1_to_s3, 2), ref::vm23_state_machine(s1_to_s3, 2));

    // s1 -> s0 (acc even): {0x90, 0x40} -> s0:acc=0x90,s=1 -> s1:acc=0x290(even),s=0
    uint8_t s1_to_s0[] = {0x90, 0x40};
    CHECK_RECORD(c, "vm23[s1->s0]", fn(s1_to_s0, 2), ref::vm23_state_machine(s1_to_s0, 2));

    // s2 -> s0 (b&0x10): {0x10, 0x10} -> s0:acc=0x10,s=2 -> s2:b=0x10,b&0x10=true,s=0
    uint8_t s2_to_s0[] = {0x10, 0x10};
    CHECK_RECORD(c, "vm23[s2->s0]", fn(s2_to_s0, 2), ref::vm23_state_machine(s2_to_s0, 2));

    // s2 -> s3 (!(b&0x10)): {0x10, 0x01} -> s0:acc=0x10,s=2 -> s2:b=0x01,b&0x10=false,s=3
    uint8_t s2_to_s3[] = {0x10, 0x01};
    CHECK_RECORD(c, "vm23[s2->s3]", fn(s2_to_s3, 2), ref::vm23_state_machine(s2_to_s3, 2));

    // s3 -> s2 (acc<=0x1000): covered above since s1_to_s3 ends in s3 with acc=0x89
    // extend: {0x81, 0x01, 0x01} -> s3:acc=(0x89<<1)|1=0x113, 0x113<=0x1000 -> s2
    uint8_t s3_to_s2[] = {0x81, 0x01, 0x01};
    CHECK_RECORD(c, "vm23[s3->s2]", fn(s3_to_s2, 3), ref::vm23_state_machine(s3_to_s2, 3));

    // s3 -> s1 (acc>0x1000): need acc > 4096 in state 3.
    // brute force: repeated shifts in state 3 grow acc exponentially.
    // {0x10, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01}
    // s0:acc=0x10,s=2 -> s2:acc=0x0F,s=3 -> s3:acc=0x1F,s=2 -> s2:acc=0x1E,s=3
    // -> s3:acc=0x3D,s=2 -> ... doubles each s3 visit until >0x1000
    uint8_t s3_to_s1[] = {0x10, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
                          0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
                          0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
                          0x01, 0x01, 0x01, 0x01, 0x01, 0x01};
    CHECK_RECORD(c, "vm23[s3->s1]", fn(s3_to_s1, 30), ref::vm23_state_machine(s3_to_s1, 30));

    // longer mixed sequences
    uint8_t seq1[] = {0x90, 0x40, 0x10, 0x01, 0xFF, 0x00, 0x80, 0x81};
    CHECK_RECORD(c, "vm23[seq1]", fn(seq1, 8), ref::vm23_state_machine(seq1, 8));

    uint8_t seq2[] = {0x81, 0x01, 0x11, 0x00, 0x82, 0x42, 0x10, 0x80,
                      0xFF, 0x7F, 0x01, 0x00, 0x90, 0x20, 0x08, 0x04};
    CHECK_RECORD(c, "vm23[seq2]", fn(seq2, 16), ref::vm23_state_machine(seq2, 16));

    // full 0-255 sweep (covers all transitions by exhaustion)
    uint8_t full[256];
    for (int i = 0; i < 256; ++i) full[i] = static_cast<uint8_t>(i);
    CHECK_RECORD(c, "vm23[0-255]", fn(full, 256), ref::vm23_state_machine(full, 256));

    // full 255-0 sweep
    uint8_t desc[256];
    for (int i = 0; i < 256; ++i) desc[i] = static_cast<uint8_t>(255 - i);
    CHECK_RECORD(c, "vm23[255-0]", fn(desc, 256), ref::vm23_state_machine(desc, 256));
}

// ===========================================================================
//  Main
// ===========================================================================

int main(int argc, char** argv)
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);

    if (argc > 2)
    {
        std::fprintf(stderr, "usage: %s [vm_test_suite library]\n", base_name(argv[0]));
        return 2;
    }

#ifdef _WIN32
    const char* dll_name = argc == 2 ? argv[1] : "vm_test_suite.dll";
#else
    const char* dll_name = argc == 2 ? argv[1] : "./build/libvm_test_suite.so";
#endif
    HMODULE dll = LoadLibraryA(dll_name);
    if (!dll)
    {
#ifdef _WIN32
        std::fprintf(stderr, "failed to load %s (GetLastError=%lu)\n", dll_name, GetLastError());
#else
        std::fprintf(stderr, "failed to load %s (dlerror=%s)\n", dll_name, g_last_load_error);
#endif
        return 2;
    }

    std::printf("testing: %s\n\n", dll_name);
    Counters c;

    test_vm01(dll, c);
    test_vm02(dll, c);
    test_vm03(dll, c);
    test_vm04(dll, c);
    test_vm05(dll, c);
    test_vm06(dll, c);
    test_vm07(dll, c);
    test_vm08(dll, c);
    test_vm09(dll, c);
    test_vm10(dll, c);
    test_vm11(dll, c);
    test_vm12(dll, c);
    test_vm13(dll, c);
    test_vm14(dll, c);
    test_vm15(dll, c);
    test_vm16(dll, c);
    test_vm17(dll, c);
    test_vm18(dll, c);
    test_vm19(dll, c);
    test_vm20(dll, c);
    test_vm21(dll, c);
    test_vm22(dll, c);
    test_vm23(dll, c);

    std::printf("\nsummary: %u passed, %u failed, %u skipped\n",
                c.passed, c.failed, c.skipped);

    FreeLibrary(dll);
    return c.failed == 0 ? 0 : 1;
}
