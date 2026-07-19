#include "dab/simd/simd.h"

#include <arm_neon.h>

static void dab_neon_complex_i16_to_f32(float *dst_i, float *dst_q, const int16_t *src, uint32_t count) {
    uint32_t i = 0u;

    for (; i + 8u <= count; i += 8u) {
        const int16x8x2_t iq = vld2q_s16(src + (2u * i));

        vst1q_f32(dst_i + i, vcvtq_f32_s32(vmovl_s16(vget_low_s16(iq.val[0]))));
        vst1q_f32(dst_i + i + 4u, vcvtq_f32_s32(vmovl_s16(vget_high_s16(iq.val[0]))));
        vst1q_f32(dst_q + i, vcvtq_f32_s32(vmovl_s16(vget_low_s16(iq.val[1]))));
        vst1q_f32(dst_q + i + 4u, vcvtq_f32_s32(vmovl_s16(vget_high_s16(iq.val[1]))));
    }

    for (; i < count; ++i) {
        dst_i[i] = (float)src[2u * i];
        dst_q[i] = (float)src[(2u * i) + 1u];
    }
}

static void dab_neon_power_i16_interleaved_f32(float *dst_power, const int16_t *src, uint32_t count) {
    uint32_t i = 0u;

    for (; i + 8u <= count; i += 8u) {
        const int16x8x2_t iq = vld2q_s16(src + (2u * i));
        const int32x4_t lo = vmlal_s16(vmull_s16(vget_low_s16(iq.val[0]), vget_low_s16(iq.val[0])),
                                       vget_low_s16(iq.val[1]),
                                       vget_low_s16(iq.val[1]));
        const int32x4_t hi = vmlal_s16(vmull_s16(vget_high_s16(iq.val[0]), vget_high_s16(iq.val[0])),
                                       vget_high_s16(iq.val[1]),
                                       vget_high_s16(iq.val[1]));

        vst1q_f32(dst_power + i, vcvtq_f32_s32(lo));
        vst1q_f32(dst_power + i + 4u, vcvtq_f32_s32(hi));
    }

    for (; i < count; ++i) {
        const float ii = (float)src[2u * i];
        const float qq = (float)src[(2u * i) + 1u];
        dst_power[i] = (ii * ii) + (qq * qq);
    }
}

static float dab_neon_dot_f32(const float *a, const float *b, uint32_t count) {
    float32x4_t acc = vdupq_n_f32(0.0f);
    uint32_t i = 0u;
    float sum;

    for (; i + 4u <= count; i += 4u) {
        acc = vfmaq_f32(acc, vld1q_f32(a + i), vld1q_f32(b + i));
    }

    sum = vaddvq_f32(acc);
    for (; i < count; ++i) {
        sum += a[i] * b[i];
    }
    return sum;
}

static void dab_neon_mul_f32(float *dst, const float *a, const float *b, uint32_t count) {
    uint32_t i = 0u;

    for (; i + 4u <= count; i += 4u) {
        vst1q_f32(dst + i, vmulq_f32(vld1q_f32(a + i), vld1q_f32(b + i)));
    }

    for (; i < count; ++i) {
        dst[i] = a[i] * b[i];
    }
}

static void dab_neon_complex_abs2_f32(float *dst_power, const float *re, const float *im, uint32_t count) {
    uint32_t i = 0u;

    for (; i + 4u <= count; i += 4u) {
        const float32x4_t rv = vld1q_f32(re + i);
        const float32x4_t iv = vld1q_f32(im + i);
        vst1q_f32(dst_power + i, vfmaq_f32(vmulq_f32(rv, rv), iv, iv));
    }

    for (; i < count; ++i) {
        dst_power[i] = (re[i] * re[i]) + (im[i] * im[i]);
    }
}

static const dab_simd_ops_t DAB_NEON_OPS = {
    DAB_SIMD_KIND_NEON,
    "neon",
    dab_neon_complex_i16_to_f32,
    dab_neon_power_i16_interleaved_f32,
    dab_neon_dot_f32,
    dab_neon_mul_f32,
    dab_neon_complex_abs2_f32
};

const dab_simd_ops_t *dab_simd_neon_ops(void) {
    return &DAB_NEON_OPS;
}
