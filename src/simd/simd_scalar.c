#include "dab/simd/simd.h"

static void dab_scalar_complex_i16_to_f32(float *dst_i, float *dst_q, const int16_t *src, uint32_t count) {
    uint32_t i;
    for (i = 0u; i < count; ++i) {
        dst_i[i] = (float)src[2u * i];
        dst_q[i] = (float)src[(2u * i) + 1u];
    }
}

static void dab_scalar_power_i16_interleaved_f32(float *dst_power, const int16_t *src, uint32_t count) {
    uint32_t i;
    for (i = 0u; i < count; ++i) {
        const float ii = (float)src[2u * i];
        const float qq = (float)src[(2u * i) + 1u];
        dst_power[i] = (ii * ii) + (qq * qq);
    }
}

static float dab_scalar_dot_f32(const float *a, const float *b, uint32_t count) {
    float sum = 0.0f;
    uint32_t i;
    for (i = 0u; i < count; ++i) {
        sum += a[i] * b[i];
    }
    return sum;
}

static void dab_scalar_mul_f32(float *dst, const float *a, const float *b, uint32_t count) {
    uint32_t i;
    for (i = 0u; i < count; ++i) {
        dst[i] = a[i] * b[i];
    }
}

static void dab_scalar_complex_abs2_f32(float *dst_power, const float *re, const float *im, uint32_t count) {
    uint32_t i;
    for (i = 0u; i < count; ++i) {
        dst_power[i] = (re[i] * re[i]) + (im[i] * im[i]);
    }
}

static const dab_simd_ops_t DAB_SCALAR_OPS = {
    DAB_SIMD_KIND_SCALAR,
    "scalar",
    dab_scalar_complex_i16_to_f32,
    dab_scalar_power_i16_interleaved_f32,
    dab_scalar_dot_f32,
    dab_scalar_mul_f32,
    dab_scalar_complex_abs2_f32
};

const dab_simd_ops_t *dab_simd_scalar_ops(void) {
    return &DAB_SCALAR_OPS;
}
