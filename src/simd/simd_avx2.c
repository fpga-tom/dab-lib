#include "dab/simd/simd.h"

#include <immintrin.h>

static void dab_avx2_complex_i16_to_f32(float *dst_i, float *dst_q, const int16_t *src, uint32_t count) {
    uint32_t i;

    for (i = 0u; i < count; ++i) {
        dst_i[i] = (float)src[2u * i];
        dst_q[i] = (float)src[(2u * i) + 1u];
    }
}

static void dab_avx2_power_i16_interleaved_f32(float *dst_power, const int16_t *src, uint32_t count) {
    uint32_t i = 0u;

    for (; i + 8u <= count; i += 8u) {
        const __m256i iq = _mm256_loadu_si256((const __m256i *)(src + (2u * i)));
        const __m256i power_i32 = _mm256_madd_epi16(iq, iq);
        const __m128i lo_i32 = _mm256_castsi256_si128(power_i32);
        const __m128i hi_i32 = _mm256_extracti128_si256(power_i32, 1);
        const __m128 lo_ps = _mm_cvtepi32_ps(lo_i32);
        const __m128 hi_ps = _mm_cvtepi32_ps(hi_i32);

        _mm_storeu_ps(dst_power + i, lo_ps);
        _mm_storeu_ps(dst_power + i + 4u, hi_ps);
    }

    for (; i < count; ++i) {
        const float ii = (float)src[2u * i];
        const float qq = (float)src[(2u * i) + 1u];
        dst_power[i] = (ii * ii) + (qq * qq);
    }
}

static float dab_avx2_dot_f32(const float *a, const float *b, uint32_t count) {
    float sum = 0.0f;
    uint32_t i = 0u;
    float lanes[8];
    __m256 acc = _mm256_setzero_ps();

    for (; i + 8u <= count; i += 8u) {
        const __m256 av = _mm256_loadu_ps(a + i);
        const __m256 bv = _mm256_loadu_ps(b + i);
        acc = _mm256_add_ps(acc, _mm256_mul_ps(av, bv));
    }

    _mm256_storeu_ps(lanes, acc);
    sum = lanes[0] + lanes[1] + lanes[2] + lanes[3] +
          lanes[4] + lanes[5] + lanes[6] + lanes[7];

    for (; i < count; ++i) {
        sum += a[i] * b[i];
    }

    return sum;
}

static const dab_simd_ops_t DAB_AVX2_OPS = {
    DAB_SIMD_KIND_AVX2,
    "avx2",
    dab_avx2_complex_i16_to_f32,
    dab_avx2_power_i16_interleaved_f32,
    dab_avx2_dot_f32
};

const dab_simd_ops_t *dab_simd_avx2_ops(void) {
    return &DAB_AVX2_OPS;
}
