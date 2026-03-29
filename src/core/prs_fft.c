#include "dab/core/prs_fft.h"

#include <math.h>

#include "dab/simd/simd.h"
#include "prs_fft_backend.h"

static const float DAB_TWO_PI_F = 6.28318530717958647692f;

static int dab_prs_fft_is_power_of_two(uint32_t n) {
    return n != 0u && (n & (n - 1u)) == 0u;
}

static uint32_t dab_prs_fft_log2(uint32_t n) {
    uint32_t bits = 0u;
    while ((1u << bits) < n) {
        bits++;
    }
    return bits;
}

static uint32_t dab_prs_fft_reverse_bits(uint32_t x, uint32_t bits) {
    uint32_t r = 0u;
    uint32_t i;

    for (i = 0u; i < bits; ++i) {
        r = (r << 1u) | ((x >> i) & 1u);
    }
    return r;
}

static const dab_prs_fft_backend_t *dab_prs_fft_select_backend(void) {
#if defined(DAB_ENABLE_AVX2) && (defined(__AVX2__) || defined(_M_AVX2))
    return dab_prs_fft_avx2_backend();
#elif defined(DAB_ENABLE_NEON) && (defined(__ARM_NEON) || defined(__ARM_NEON__))
    return dab_prs_fft_neon_backend();
#else
    return dab_prs_fft_scalar_backend();
#endif
}

void dab_prs_fft_default_config(dab_prs_fft_config_t *config) {
    if (config == 0) {
        return;
    }
    config->fft_samples = 2048u;
    config->sample_rate = 2048000u;
}

int dab_prs_fft_init(dab_prs_fft_t *fft, const dab_prs_fft_config_t *config) {
    dab_prs_fft_config_t defaults;
    const dab_prs_fft_backend_t *backend;
    uint32_t bits;
    uint32_t i;
    uint32_t len;
    uint32_t offset = 0u;

    if (fft == 0) {
        return -1;
    }

    dab_prs_fft_default_config(&defaults);
    fft->config = config != 0 ? *config : defaults;
    if (fft->config.fft_samples == 0u ||
        fft->config.sample_rate == 0u ||
        fft->config.fft_samples > DAB_PRS_FFT_MAX_SAMPLES ||
        !dab_prs_fft_is_power_of_two(fft->config.fft_samples)) {
        return -1;
    }

    backend = dab_prs_fft_select_backend();
    fft->backend_kind = backend->kind;
    fft->backend_name = backend->name;

    bits = dab_prs_fft_log2(fft->config.fft_samples);
    for (i = 0u; i < fft->config.fft_samples; ++i) {
        fft->bitrev[i] = dab_prs_fft_reverse_bits(i, bits);
    }

    fft->stage_count = 0u;
    for (len = 2u; len <= fft->config.fft_samples; len <<= 1u) {
        const uint32_t half = len >> 1u;
        uint32_t j;

        if (fft->stage_count >= DAB_PRS_FFT_MAX_STAGES || (offset + half) > (DAB_PRS_FFT_MAX_SAMPLES - 1u)) {
            return -1;
        }

        fft->stage_offsets[fft->stage_count] = offset;
        fft->stage_halves[fft->stage_count] = half;
        for (j = 0u; j < half; ++j) {
            const float phase = -DAB_TWO_PI_F * (float)j / (float)len;
            fft->twiddle_re[offset + j] = cosf(phase);
            fft->twiddle_im[offset + j] = sinf(phase);
        }
        offset += half;
        fft->stage_count++;
    }

    return 0;
}

void dab_prs_fft_reset(dab_prs_fft_t *fft) {
    (void)fft;
}

static void dab_prs_fft_bit_reverse_permute(const dab_prs_fft_t *fft, float *re, float *im) {
    uint32_t i;

    for (i = 0u; i < fft->config.fft_samples; ++i) {
        const uint32_t j = fft->bitrev[i];
        if (j > i) {
            const float tr = re[i];
            const float ti = im[i];
            re[i] = re[j];
            im[i] = im[j];
            re[j] = tr;
            im[j] = ti;
        }
    }
}

int dab_prs_fft_execute_i16_cfo(const dab_prs_fft_t *fft,
                                const dab_sdr_block_view_t *block,
                                uint64_t fft_start_sample,
                                float cfo_hz,
                                float *out_i,
                                float *out_q) {
    const dab_prs_fft_backend_t *backend = dab_prs_fft_select_backend();
    const uint64_t block_start = block != 0 ? block->sample_index : 0u;
    uint32_t offset;
    uint32_t stage;

    if (fft == 0 || block == 0 || out_i == 0 || out_q == 0 || block->iq_interleaved == 0) {
        return -1;
    }
    if (fft_start_sample < block_start) {
        return -1;
    }

    offset = (uint32_t)(fft_start_sample - block_start);
    if ((offset + fft->config.fft_samples) > block->sample_count) {
        return -1;
    }

    dab_simd_get_ops()->complex_i16_to_f32(out_i,
                                           out_q,
                                           block->iq_interleaved + (2u * offset),
                                           fft->config.fft_samples);
    if (cfo_hz != 0.0f) {
        const double step = -(2.0 * 3.14159265358979323846 * (double)cfo_hz) /
                            (double)fft->config.sample_rate;
        const double base_phase =
            remainder(step * (double)fft_start_sample, 2.0 * 3.14159265358979323846);
        const float step_re = (float)cos(step);
        const float step_im = (float)sin(step);
        float rot_re = (float)cos(base_phase);
        float rot_im = (float)sin(base_phase);
        uint32_t n;

        for (n = 0u; n < fft->config.fft_samples; ++n) {
            const float in_re = out_i[n];
            const float in_im = out_q[n];
            const float next_re = (rot_re * step_re) - (rot_im * step_im);
            const float next_im = (rot_re * step_im) + (rot_im * step_re);

            out_i[n] = (in_re * rot_re) - (in_im * rot_im);
            out_q[n] = (in_re * rot_im) + (in_im * rot_re);
            rot_re = next_re;
            rot_im = next_im;
        }
    }
    dab_prs_fft_bit_reverse_permute(fft, out_i, out_q);

    for (stage = 0u; stage < fft->stage_count; ++stage) {
        backend->stage_fn(out_i,
                          out_q,
                          fft->twiddle_re + fft->stage_offsets[stage],
                          fft->twiddle_im + fft->stage_offsets[stage],
                          fft->config.fft_samples,
                          fft->stage_halves[stage]);
    }

    return 0;
}

int dab_prs_fft_execute_i16(const dab_prs_fft_t *fft,
                            const dab_sdr_block_view_t *block,
                            uint64_t fft_start_sample,
                            float *out_i,
                            float *out_q) {
    return dab_prs_fft_execute_i16_cfo(fft, block, fft_start_sample, 0.0f, out_i, out_q);
}

int dab_prs_fft_apply_bin_shift(float *re,
                                float *im,
                                uint32_t count,
                                int32_t shift_bins) {
    uint32_t i;
    uint32_t shift;
    float tmp_re[DAB_PRS_FFT_MAX_SAMPLES];
    float tmp_im[DAB_PRS_FFT_MAX_SAMPLES];

    if (re == 0 || im == 0 || count == 0u || count > DAB_PRS_FFT_MAX_SAMPLES) {
        return -1;
    }

    if (shift_bins == 0) {
        return 0;
    }

    shift = (uint32_t)(((shift_bins % (int32_t)count) + (int32_t)count) % (int32_t)count);
    for (i = 0u; i < count; ++i) {
        const uint32_t dst = (i + shift) % count;
        tmp_re[dst] = re[i];
        tmp_im[dst] = im[i];
    }
    for (i = 0u; i < count; ++i) {
        re[i] = tmp_re[i];
        im[i] = tmp_im[i];
    }

    return 0;
}
