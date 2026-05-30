#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

#define LOKI_IS_ZERO64(value) (((((value) | (0ull - (value))) >> 63) ^ 1ull) & 1ull)
#define LOKI_MASK_EQ(value, key) (0ull - LOKI_IS_ZERO64((value) ^ static_cast<uint64_t>(key)))
#define LOKI_ULT64(lhs, rhs) (((~(lhs) & (rhs)) | (~((lhs) ^ (rhs)) & ((lhs) - (rhs)))) >> 63)
#define LOKI_SLT64(lhs, rhs) ((((lhs) >> 63) & ~((rhs) >> 63)) | (((((lhs) >> 63) ^ ((rhs) >> 63)) ^ 1ull) & LOKI_ULT64((lhs), (rhs))))

static constexpr uint64_t kEncodeOpXor = 0x6085fd98647c18d1ull;
static constexpr uint64_t kEncodeAXor = 0x61eb9df41527d67aull;
static constexpr uint64_t kEncodeBXor = 0x9533ddde0433857eull;
static constexpr uint64_t kEncodeCXor = 0x4c07bd05d235fb7full;

static constexpr uint64_t kOpAdd = 0x858fa6e874bff19dull;
static constexpr uint64_t kOpSub = 0x9bb8761b19cb0e5eull;
static constexpr uint64_t kOpXor = 0xdc7d2ef682808690ull;
static constexpr uint64_t kOpAnd = 0x48bcc4aaeb6de9edull;
static constexpr uint64_t kOpOr = 0x8711f219609f78abull;
static constexpr uint64_t kOpMul = 0x5d47789a1620f932ull;
static constexpr uint64_t kOpLShr = 0x413379bc09a169b3ull;
static constexpr uint64_t kOpShl = 0x4a442bed4f44eaa9ull;
static constexpr uint64_t kOpNot = 0xa5e6b37ab2f8a499ull;
static constexpr uint64_t kOpUDiv = 0x1a9a4efc2a87ed7dull;
static constexpr uint64_t kOpURem = 0x030cdecd19d256c8ull;
static constexpr uint64_t kOpEq = 0x4dadc309c09a5a0aull;
static constexpr uint64_t kOpIsZero = 0x9e1a81e5ea0d6be4ull;
static constexpr uint64_t kOpSelect = 0xa339eb0bdf30a651ull;
static constexpr uint64_t kOpAddC = 0xab73e263c08dd9f1ull;
static constexpr uint64_t kOpXorC = 0xa747760865de29e9ull;
static constexpr uint64_t kOpNe = 0xfd5c00ef468f3151ull;
static constexpr uint64_t kOpUlt = 0x0c8536dd3513962full;
static constexpr uint64_t kOpUle = 0x2521ac88e5896054ull;
static constexpr uint64_t kOpUgt = 0x93f8299a972f1dfeull;
static constexpr uint64_t kOpUge = 0xfda8e89e839ab4b5ull;
static constexpr uint64_t kOpSlt = 0x3d72c129e4374445ull;
static constexpr uint64_t kOpSle = 0xeca71578432b1fd9ull;
static constexpr uint64_t kOpSgt = 0xd693de88938b7c39ull;
static constexpr uint64_t kOpSge = 0xdc1ab69661ef5675ull;
static constexpr uint64_t kOpAShr = 0x238b9dba71e09f4aull;
static constexpr uint64_t kOpSDiv = 0xed434e006bad3d64ull;
static constexpr uint64_t kOpSRem = 0x2ea46a87a27393f0ull;

extern "C" uint64_t target_function(uint64_t encoded_op, uint64_t encoded_a, uint64_t encoded_b, uint64_t encoded_c)
{
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
        result = target_function(op ^ kEncodeOpXor, a ^ kEncodeAXor, b ^ kEncodeBXor, c ^ kEncodeCXor);
        auto t2 = std::chrono::high_resolution_clock::now();
        duration_sum += std::chrono::duration<double, std::micro>(t2 - t1).count();
    }

    std::printf("Output: %llu\nTime: %lfms\n",
                static_cast<unsigned long long>(result),
                duration_sum / 10000);
    return 0;
}
