#include "prs_fft_backend.h"

#include <arm_neon.h>

static void dab_prs_fft_neon_stage(float *re,
                                   float *im,
                                   const float *tw_re,
                                   const float *tw_im,
                                   uint32_t fft_samples,
                                   uint32_t half_size) {
    const uint32_t len = half_size << 1u;
    uint32_t group;
    uint32_t j;

    for (group = 0u; group < fft_samples; group += len) {
        for (j = 0u; j + 4u <= half_size; j += 4u) {
            const uint32_t a = group + j;
            const uint32_t b = a + half_size;
            const float32x4_t wre = vld1q_f32(tw_re + j);
            const float32x4_t wim = vld1q_f32(tw_im + j);
            const float32x4_t ure = vld1q_f32(re + a);
            const float32x4_t uim = vld1q_f32(im + a);
            const float32x4_t vre = vld1q_f32(re + b);
            const float32x4_t vim = vld1q_f32(im + b);
            const float32x4_t tre = vsubq_f32(vmulq_f32(vre, wre), vmulq_f32(vim, wim));
            const float32x4_t tim = vaddq_f32(vmulq_f32(vre, wim), vmulq_f32(vim, wre));

            vst1q_f32(re + a, vaddq_f32(ure, tre));
            vst1q_f32(im + a, vaddq_f32(uim, tim));
            vst1q_f32(re + b, vsubq_f32(ure, tre));
            vst1q_f32(im + b, vsubq_f32(uim, tim));
        }

        for (; j < half_size; ++j) {
            const uint32_t a = group + j;
            const uint32_t b = a + half_size;
            const float w_re = tw_re[j];
            const float w_im = tw_im[j];
            const float u_re = re[a];
            const float u_im = im[a];
            const float v_re = re[b];
            const float v_im = im[b];
            const float t_re = (v_re * w_re) - (v_im * w_im);
            const float t_im = (v_re * w_im) + (v_im * w_re);

            re[a] = u_re + t_re;
            im[a] = u_im + t_im;
            re[b] = u_re - t_re;
            im[b] = u_im - t_im;
        }
    }
}

static const dab_prs_fft_backend_t DAB_PRS_FFT_NEON_BACKEND = {
    DAB_PRS_FFT_BACKEND_NEON,
    "neon",
    dab_prs_fft_neon_stage
};

const dab_prs_fft_backend_t *dab_prs_fft_neon_backend(void) {
    return &DAB_PRS_FFT_NEON_BACKEND;
}
