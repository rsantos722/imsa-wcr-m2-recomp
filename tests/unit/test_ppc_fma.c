// test_ppc_fma.c — guest fused multiply-add rounding.
//
// FMADDS and its variants must round a*b+c once. Expected values come from
// integer arithmetic over the whole float range, not the host's fmaf(), whose
// rounding differs between C runtimes and CPUs.
#include "ppc_ctx.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int failures;

#define CHECK(expr) do {                                                    \
    if (!(expr)) {                                                          \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr);     \
        ++failures;                                                         \
    }                                                                       \
} while (0)

static uint32_t float_bits(float f) {
    uint32_t u;
    memcpy(&u, &f, sizeof(u));
    return u;
}

static float bits_float(uint32_t u) {
    float f;
    memcpy(&f, &u, sizeof(f));
    return f;
}

// ---- exact oracle ----------------------------------------------------------
// Magnitudes are integers in units of 2^-298, the lowest bit a product of two
// float subnormals can have. Sums stay below 2^257, so 555 bits are enough.
enum { BIG_WORDS = 18, BIG_UNIT_EXP = -298, FLOAT_LSB_BIT = 149 };

typedef struct {
    uint32_t w[BIG_WORDS];
} big;

// Splits a finite float into sign and m * 2^e.
static void float_parts(float f, int* neg, uint32_t* m, int* e) {
    const uint32_t u = float_bits(f);
    const int exp_field = (int)((u >> 23) & 0xFFu);
    const uint32_t frac = u & 0x007FFFFFu;
    *neg = (int)(u >> 31);
    *m = exp_field ? (frac | 0x00800000u) : frac;
    *e = (exp_field ? exp_field : 1) - 150;
}

static void big_from_shifted(big* out, uint64_t value, int shift) {
    const int word = shift / 32;
    const int bit = shift % 32;
    int i = word;
    memset(out, 0, sizeof(*out));
    out->w[i++] = (uint32_t)(value << bit);
    value = bit ? value >> (32 - bit) : value >> 32;
    while (value) {
        out->w[i++] = (uint32_t)value;
        value >>= 32;
    }
}

static int big_cmp(const big* a, const big* b) {
    for (int i = BIG_WORDS - 1; i >= 0; i--) {
        if (a->w[i] != b->w[i]) return a->w[i] > b->w[i] ? 1 : -1;
    }
    return 0;
}

static void big_add(big* out, const big* a, const big* b) {
    uint64_t carry = 0;
    for (int i = 0; i < BIG_WORDS; i++) {
        carry += (uint64_t)a->w[i] + b->w[i];
        out->w[i] = (uint32_t)carry;
        carry >>= 32;
    }
}

static void big_sub(big* out, const big* a, const big* b) {
    uint64_t borrow = 0;
    for (int i = 0; i < BIG_WORDS; i++) {
        const uint64_t d = (uint64_t)a->w[i] - b->w[i] - borrow;
        out->w[i] = (uint32_t)d;
        borrow = (d >> 32) & 1u;
    }
}

static int big_bit(const big* a, int bit) {
    return (int)((a->w[bit / 32] >> (bit % 32)) & 1u);
}

static int big_msb(const big* a) {
    for (int i = BIG_WORDS - 1; i >= 0; i--) {
        if (a->w[i]) {
            int b = 31;
            while (!((a->w[i] >> b) & 1u)) b--;
            return i * 32 + b;
        }
    }
    return -1;
}

// Rounds to float with ties to even, assembling the IEEE encoding directly.
static float big_round(const big* a, int neg) {
    const uint32_t sign = neg ? 0x80000000u : 0u;
    const int msb = big_msb(a);
    int lsb = msb - 23;
    uint32_t m = 0;
    int round_bit;
    int sticky = 0;
    uint32_t encoded;
    if (lsb < FLOAT_LSB_BIT) lsb = FLOAT_LSB_BIT;
    for (int i = msb; i >= lsb; i--) m = (m << 1) | (uint32_t)big_bit(a, i);
    round_bit = big_bit(a, lsb - 1);
    for (int i = lsb - 2; i >= 0 && !sticky; i--) sticky = big_bit(a, i);
    if (round_bit && (sticky || (m & 1u))) m++;
    if (m == 0) return bits_float(sign);
    if (lsb == FLOAT_LSB_BIT && m < 0x01000000u) {
        // Exponent field 1 with a hidden bit, or a subnormal: the raw value is
        // already the encoding, because a carry into bit 23 raises the field.
        return bits_float(sign | m);
    }
    if (m == 0x01000000u) {
        m >>= 1;
        lsb += 1;
    }
    const int exp_field = lsb - FLOAT_LSB_BIT + 1;
    if (exp_field >= 255) return bits_float(sign | 0x7F800000u);
    encoded = ((uint32_t)exp_field << 23) | (m & 0x007FFFFFu);
    return bits_float(sign | encoded);
}

// Exact a*b+c for finite inputs.
static float exact_fma(float a, float b, float cc) {
    int neg_a, neg_b, neg_c, exp_a, exp_b, exp_c;
    uint32_t m_a, m_b, m_c;
    big p, c, r;
    float_parts(a, &neg_a, &m_a, &exp_a);
    float_parts(b, &neg_b, &m_b, &exp_b);
    float_parts(cc, &neg_c, &m_c, &exp_c);
    const int neg_p = neg_a ^ neg_b;
    const uint64_t m_p = (uint64_t)m_a * m_b;
    if (m_p == 0 && m_c == 0) return bits_float((neg_p && neg_c) ? 0x80000000u : 0u);
    if (m_p == 0) return cc;
    big_from_shifted(&p, m_p, exp_a + exp_b - BIG_UNIT_EXP);
    big_from_shifted(&c, m_c, exp_c - BIG_UNIT_EXP);
    if (neg_p == neg_c) {
        big_add(&r, &p, &c);
        return big_round(&r, neg_p);
    }
    const int order = big_cmp(&p, &c);
    if (order == 0) return 0.0f;
    if (order > 0) {
        big_sub(&r, &p, &c);
        return big_round(&r, neg_p);
    }
    big_sub(&r, &c, &p);
    return big_round(&r, neg_c);
}

// ---- deterministic inputs --------------------------------------------------
static uint64_t rng_state = 0x9E3779B97F4A7C15ull;

static uint32_t next_u32(void) {
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 7;
    rng_state ^= rng_state << 17;
    return (uint32_t)(rng_state >> 16);
}

static float make_finite(uint32_t sign, int exp_field, uint32_t frac) {
    return bits_float((sign << 31) | ((uint32_t)exp_field << 23) | (frac & 0x007FFFFFu));
}

static float random_finite(void) {
    const uint32_t r = next_u32();
    const int exp_field = (int)(next_u32() % 255u);
    return make_finite(r >> 31, exp_field, r);
}

// Exponents near 1.0, where products and addends overlap the most.
static float random_mid(int spread) {
    const uint32_t r = next_u32();
    const int exp_field = 127 - spread + (int)(next_u32() % (uint32_t)(2 * spread + 1));
    return make_finite(r >> 31, exp_field, r);
}

static float scale_by(float f, int power) {
    return (float)ldexp((double)f, power);
}

static uint64_t compared;

static void check_one(float a, float b, float cc) {
    const float want = exact_fma(a, b, cc);
    const float got = FMADDS(a, b, cc);
    compared++;
    if (float_bits(want) != float_bits(got)) {
        ++failures;
        if (failures <= 20) {
            fprintf(stderr, "FAIL FMADDS(%a, %a, %a): got %a want %a\n",
                    (double)a, (double)b, (double)cc, (double)got, (double)want);
        }
    }
}

// ---- tests -----------------------------------------------------------------
static void test_double_rounding_trap(void) {
    // a*b+c = 1 + 2^-23 + 2^-24 - 2^-60, just below a float tie. A plain double
    // add rounds to the tie, and narrowing then picks the wrong even neighbor.
    const float a = 0x1.00004p-12f;
    const float b = 0x1.ffff8p-13f;
    const float c = 0x1.000002p0f;
    CHECK(float_bits(FMADDS(a, b, c)) == float_bits(0x1.000002p0f));
    CHECK(float_bits((float)((double)a * (double)b + (double)c)) == float_bits(0x1.000004p0f));
    CHECK(float_bits(exact_fma(a, b, c)) == float_bits(0x1.000002p0f));
    CHECK(float_bits(FMADDS(-a, b, -c)) == float_bits(-0x1.000002p0f));
}

static void test_oracle_known_values(void) {
    CHECK(float_bits(exact_fma(2.0f, 3.0f, 4.0f)) == float_bits(10.0f));
    CHECK(float_bits(exact_fma(0x1p-149f, 0.5f, 0.0f)) == float_bits(0.0f));
    CHECK(float_bits(exact_fma(0x1p-149f, 0.75f, 0.0f)) == float_bits(0x1p-149f));
    CHECK(float_bits(exact_fma(0x1p-126f, 1.0f, -0x1p-149f)) == float_bits(0x1.fffffcp-127f));
    CHECK(float_bits(exact_fma(0x1.fffffcp-127f, 1.0f, 0x1p-149f)) == float_bits(0x1p-126f));
    CHECK(float_bits(exact_fma(0x1.fffffep127f, 2.0f, 0.0f)) == float_bits((float)INFINITY));
    CHECK(float_bits(exact_fma(-0x1.fffffep127f, 2.0f, 0.0f)) == float_bits(-(float)INFINITY));
}

static void test_signed_zero(void) {
    CHECK(float_bits(FMADDS(1.0f, 1.0f, -1.0f)) == float_bits(0.0f));
    CHECK(float_bits(FMADDS(-0.0f, 1.0f, -0.0f)) == float_bits(-0.0f));
    CHECK(float_bits(FMADDS(0.0f, 1.0f, -0.0f)) == float_bits(0.0f));
    CHECK(float_bits(FMADDS(-0.0f, 1.0f, 0.0f)) == float_bits(0.0f));
    CHECK(float_bits(FMADDS(0.0f, 1.0f, 3.0f)) == float_bits(3.0f));
}

static void test_subnormal_and_overflow(void) {
    // 2^-149 * 0.5 is a tie between 0 and the smallest subnormal.
    CHECK(float_bits(FMADDS(0x1p-149f, 0.5f, 0.0f)) == float_bits(0.0f));
    CHECK(float_bits(FMADDS(0x1p-149f, 0x1.000002p-1f, 0.0f)) == float_bits(0x1p-149f));
    CHECK(float_bits(FMADDS(-0x1p-149f, 0.25f, -0.0f)) == float_bits(-0.0f));
    CHECK(float_bits(FMADDS(0x1.fffffep127f, 2.0f, 0.0f)) == float_bits((float)INFINITY));
    CHECK(float_bits(FMADDS(-0x1.fffffep127f, 2.0f, 0.0f)) == float_bits(-(float)INFINITY));
    // Cancellation can bring an overflowing product back into range.
    CHECK(float_bits(FMADDS(0x1p127f, 2.0f, -0x1p127f)) == float_bits(0x1p127f));
}

static void test_non_finite(void) {
    const float inf = (float)INFINITY;
    const float nan = (float)NAN;
    CHECK(float_bits(FMADDS(inf, 2.0f, 1.0f)) == float_bits(inf));
    CHECK(float_bits(FMADDS(-inf, 2.0f, 1.0f)) == float_bits(-inf));
    CHECK(float_bits(FMADDS(1.0f, 2.0f, -inf)) == float_bits(-inf));
    CHECK(isnan(FMADDS(inf, 0.0f, 1.0f)));
    CHECK(isnan(FMADDS(inf, 1.0f, -inf)));
    CHECK(isnan(FMADDS(nan, 1.0f, 1.0f)));
    CHECK(isnan(FMADDS(1.0f, nan, 1.0f)));
    CHECK(isnan(FMADDS(1.0f, 1.0f, nan)));
}

static void test_variants(void) {
    const float a = 0x1.00004p-12f;
    const float b = 0x1.ffff8p-13f;
    const float c = 0x1.000002p0f;
    CHECK(float_bits(FMSUBS(a, b, -c)) == float_bits(0x1.000002p0f));
    CHECK(float_bits(FNMADDS(a, b, c)) == float_bits(-0x1.000002p0f));
    CHECK(float_bits(FNMSUBS(a, b, -c)) == float_bits(-0x1.000002p0f));
    // Negating the rounded result differs from negating one term.
    CHECK(float_bits(FNMADDS(1.0f, 1.0f, -1.0f)) == float_bits(-0.0f));
    CHECK(float_bits(FNMSUBS(1.0f, 1.0f, 1.0f)) == float_bits(-0.0f));
    CHECK(float_bits(FMSUBS(1.0f, 1.0f, 1.0f)) == float_bits(0.0f));
}

static void test_generated_inputs(void) {
    enum { ROUNDS = 400000 };
    for (int i = 0; i < ROUNDS; i++) {
        // Anywhere in the float range.
        check_one(random_finite(), random_finite(), random_finite());

        // Overlapping magnitudes.
        const float a = random_mid(4);
        const float b = random_mid(4);
        check_one(a, b, random_mid(8));

        // Near-total cancellation against the rounded product.
        const float rounded = (float)((double)a * (double)b);
        const uint32_t nudge = next_u32() % 5u;
        check_one(a, b, bits_float(float_bits(-rounded) + nudge));

        // Product near half an ulp of c, which is where double rounding fails:
        // (1 + x)(1 - x) has bits far below the 24 kept by c's significand.
        const uint32_t x_bits = next_u32() % 23u;
        const float x = scale_by(1.0f, -(int)x_bits - 1);
        const int c_exp = (int)(next_u32() % 40u) - 20;
        const float c = scale_by(make_finite(next_u32() & 1u, 127, next_u32() | 1u), c_exp);
        const float half_ulp = scale_by(1.0f, c_exp - 24);
        check_one((1.0f + x) * scale_by(half_ulp, (int)(next_u32() % 3u) - 1),
                  1.0f - x, c);
        check_one((1.0f + x) * scale_by(half_ulp, (int)(next_u32() % 3u) - 1),
                  1.0f + x, c);

        // Results that land in or near the subnormal range.
        const float tiny_a = make_finite(next_u32() & 1u, 1 + (int)(next_u32() % 60u), next_u32());
        const float tiny_b = make_finite(next_u32() & 1u, 64 + (int)(next_u32() % 60u), next_u32());
        const float tiny_c = make_finite(next_u32() & 1u, (int)(next_u32() % 4u), next_u32());
        check_one(tiny_a, tiny_b, tiny_c);
    }
}

int main(void) {
    test_oracle_known_values();
    test_double_rounding_trap();
    test_signed_zero();
    test_subnormal_and_overflow();
    test_non_finite();
    test_variants();
    test_generated_inputs();
    if (failures) {
        fprintf(stderr, "m2_ppc_fma: %d failure(s) in %llu comparisons\n",
                failures, (unsigned long long)compared);
        return 1;
    }
    printf("m2_ppc_fma: ok (%llu comparisons)\n", (unsigned long long)compared);
    return 0;
}
