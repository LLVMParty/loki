#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

#define LOKI_IS_ZERO64(value) (((((value) | (0ull - (value))) >> 63) ^ 1ull) & 1ull)
#define LOKI_MASK_EQ(value, key) (0ull - LOKI_IS_ZERO64((value) ^ static_cast<uint64_t>(key)))
#define LOKI_ULT64(lhs, rhs) (((~(lhs) & (rhs)) | (~((lhs) ^ (rhs)) & ((lhs) - (rhs)))) >> 63)
#define LOKI_SLT64(lhs, rhs) ((((lhs) >> 63) & ~((rhs) >> 63)) | (((((lhs) >> 63) ^ ((rhs) >> 63)) ^ 1ull) & LOKI_ULT64((lhs), (rhs))))

extern "C" uint64_t target_function(uint64_t op, uint64_t a, uint64_t b, uint64_t c)
{
    uint64_t safe_b = b | LOKI_IS_ZERO64(b);
    uint64_t sh = b & 63ull;
    uint64_t sel_mask = 0ull - (c & 1ull);

    uint64_t r = 0;
    r |= LOKI_MASK_EQ(op, 0)  & (a + b);
    r |= LOKI_MASK_EQ(op, 1)  & (a - b);
    r |= LOKI_MASK_EQ(op, 2)  & (a ^ b);
    r |= LOKI_MASK_EQ(op, 3)  & (a & b);
    r |= LOKI_MASK_EQ(op, 4)  & (a | b);
    r |= LOKI_MASK_EQ(op, 5)  & (a * b);
    r |= LOKI_MASK_EQ(op, 6)  & (a >> sh);
    r |= LOKI_MASK_EQ(op, 7)  & (a << sh);
    r |= LOKI_MASK_EQ(op, 8)  & (~a);
    r |= LOKI_MASK_EQ(op, 9)  & (a / safe_b);
    r |= LOKI_MASK_EQ(op, 10) & (a % safe_b);
    r |= LOKI_MASK_EQ(op, 11) & LOKI_IS_ZERO64(a ^ b);
    r |= LOKI_MASK_EQ(op, 12) & LOKI_IS_ZERO64(a);
    r |= LOKI_MASK_EQ(op, 13) & ((a & sel_mask) | (b & ~sel_mask));

    uint64_t eq = LOKI_IS_ZERO64(a ^ b);
    uint64_t ult = LOKI_ULT64(a, b);
    uint64_t ugt = LOKI_ULT64(b, a);
    uint64_t slt = LOKI_SLT64(a, b);
    uint64_t sgt = LOKI_SLT64(b, a);

    r |= LOKI_MASK_EQ(op, 14) & (a + c);
    r |= LOKI_MASK_EQ(op, 15) & (a ^ c);
    r |= LOKI_MASK_EQ(op, 16) & (eq ^ 1ull);
    r |= LOKI_MASK_EQ(op, 17) & ult;
    r |= LOKI_MASK_EQ(op, 18) & (ult | eq);
    r |= LOKI_MASK_EQ(op, 19) & ugt;
    r |= LOKI_MASK_EQ(op, 20) & (ugt | eq);
    r |= LOKI_MASK_EQ(op, 21) & slt;
    r |= LOKI_MASK_EQ(op, 22) & (slt | eq);
    r |= LOKI_MASK_EQ(op, 23) & sgt;
    r |= LOKI_MASK_EQ(op, 24) & (sgt | eq);
    r |= LOKI_MASK_EQ(op, 25) & static_cast<uint64_t>(static_cast<int64_t>(a) >> sh);
    r |= LOKI_MASK_EQ(op, 26) & (a / safe_b);
    r |= LOKI_MASK_EQ(op, 27) & (a % safe_b);
    return r;
}

static uint64_t parse_u64(const char* text)
{
    return std::strtoull(text, nullptr, 0);
}

int main(int argc, char** argv)
{
    uint64_t op = argc > 1 ? parse_u64(argv[1]) : 0;
    uint64_t a = argc > 2 ? parse_u64(argv[2]) : 1;
    uint64_t b = argc > 3 ? parse_u64(argv[3]) : 2;
    uint64_t c = argc > 4 ? parse_u64(argv[4]) : 0;

    double duration_sum = 0;
    uint64_t result = 0;
    for (int i = 0; i < 10000; ++i)
    {
        auto t1 = std::chrono::high_resolution_clock::now();
        result = target_function(op, a, b, c);
        auto t2 = std::chrono::high_resolution_clock::now();
        duration_sum += std::chrono::duration<double, std::micro>(t2 - t1).count();
    }

    std::printf("Output: %llu\nTime: %lfms\n",
                static_cast<unsigned long long>(result),
                duration_sum / 10000);
    return 0;
}
