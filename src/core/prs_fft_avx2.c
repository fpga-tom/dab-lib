#include "prs_fft_backend.h"

#include <immintrin.h>

static void dab_prs_fft_avx2_stage(float *re,
                                   float *im,
                                   const float *tw_re,
                                   const float *tw_im,
                                   uint32_t fft_samples,
                                   uint32_t half_size) {
    const uint32_t len = half_size << 1u;
    uint32_t group;
    uint32_t j;

    for (group = 0u; group < fft_samples; group += len) {
        for (j = 0u; j + 8u <= half_size; j += 8u) {
            const uint32_t a = group + j;
            const uint32_t b = a + half_size;
            const __m256 wre = _mm256_loadu_ps(tw_re + j);
            const __m256 wim = _mm256_loadu_ps(tw_im + j);
            const __m256 ure = _mm256_loadu_ps(re + a);
            const __m256 uim = _mm256_loadu_ps(im + a);
            const __m256 vre = _mm256_loadu_ps(re + b);
            const __m256 vim = _mm256_loadu_ps(im + b);
            const __m256 tre = _mm256_sub_ps(_mm256_mul_ps(vre, wre), _mm256_mul_ps(vim, wim));
            const __m256 tim = _mm256_add_ps(_mm256_mul_ps(vre, wim), _mm256_mul_ps(vim, wre));

            _mm256_storeu_ps(re + a, _mm256_add_ps(ure, tre));
            _mm256_storeu_ps(im + a, _mm256_add_ps(uim, tim));
            _mm256_storeu_ps(re + b, _mm256_sub_ps(ure, tre));
            _mm256_storeu_ps(im + b, _mm256_sub_ps(uim, tim));
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

static const dab_prs_fft_backend_t DAB_PRS_FFT_AVX2_BACKEND = {
    DAB_PRS_FFT_BACKEND_AVX2,
    "avx2",
    dab_prs_fft_avx2_stage
};

const dab_prs_fft_backend_t *dab_prs_fft_avx2_backend(void) {
    return &DAB_PRS_FFT_AVX2_BACKEND;
}
