#ifndef DAB_CORE_PRS_FFT_H
#define DAB_CORE_PRS_FFT_H

#include <stdint.h>

#include "dab/hal/sdr_device.h"

enum {
    DAB_PRS_FFT_MAX_SAMPLES = 2048u,
    DAB_PRS_FFT_MAX_STAGES = 16u
};

typedef enum {
    DAB_PRS_FFT_BACKEND_SCALAR = 0,
    DAB_PRS_FFT_BACKEND_AVX2 = 1,
    DAB_PRS_FFT_BACKEND_NEON = 2
} dab_prs_fft_backend_kind_t;

typedef struct {
    uint32_t fft_samples;
    uint32_t sample_rate;
} dab_prs_fft_config_t;

typedef struct {
    dab_prs_fft_config_t config;
    dab_prs_fft_backend_kind_t backend_kind;
    const char *backend_name;
    uint32_t stage_count;
    uint32_t bitrev[DAB_PRS_FFT_MAX_SAMPLES];
    uint32_t stage_offsets[DAB_PRS_FFT_MAX_STAGES];
    uint32_t stage_halves[DAB_PRS_FFT_MAX_STAGES];
    float twiddle_re[DAB_PRS_FFT_MAX_SAMPLES - 1u];
    float twiddle_im[DAB_PRS_FFT_MAX_SAMPLES - 1u];
} dab_prs_fft_t;

void dab_prs_fft_default_config(dab_prs_fft_config_t *config);
int dab_prs_fft_init(dab_prs_fft_t *fft, const dab_prs_fft_config_t *config);
void dab_prs_fft_reset(dab_prs_fft_t *fft);

int dab_prs_fft_execute_i16(const dab_prs_fft_t *fft,
                            const dab_sdr_block_view_t *block,
                            uint64_t fft_start_sample,
                            float *out_i,
                            float *out_q);
int dab_prs_fft_execute_i16_cfo(const dab_prs_fft_t *fft,
                                const dab_sdr_block_view_t *block,
                                uint64_t fft_start_sample,
                                float cfo_hz,
                                float *out_i,
                                float *out_q);
int dab_prs_fft_execute_complex_f32(const dab_prs_fft_t *fft,
                                    float *re,
                                    float *im,
                                    int inverse);
int dab_prs_fft_apply_bin_shift(float *re,
                                float *im,
                                uint32_t count,
                                int32_t shift_bins);

#endif
