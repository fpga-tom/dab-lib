#ifndef DAB_CORE_OFDM_H
#define DAB_CORE_OFDM_H

#include <stdint.h>

enum {
    DAB_MODE_I_FFT_SAMPLES = 2048u,
    DAB_MODE_I_GUARD_SAMPLES = 504u,
    DAB_MODE_I_SYMBOL_SAMPLES = 2552u,
    DAB_MODE_I_ACTIVE_CARRIERS = 1537u,
    DAB_MODE_I_DATA_CARRIERS = 1536u,
    DAB_MODE_I_ACTIVE_CARRIER_HALF = 768u
};

typedef struct {
    uint32_t fft_samples;
    uint32_t active_carriers;
    uint32_t data_carriers;
} dab_ofdm_config_t;

typedef struct {
    dab_ofdm_config_t config;
} dab_ofdm_t;

void dab_ofdm_default_config(dab_ofdm_config_t *config);
int dab_ofdm_init(dab_ofdm_t *ofdm, const dab_ofdm_config_t *config);
void dab_ofdm_reset(dab_ofdm_t *ofdm);

int dab_ofdm_extract_active_carriers_mode_i(const float *fft_i,
                                            const float *fft_q,
                                            float *active_i,
                                            float *active_q);
int dab_ofdm_diff_demodulate_mode_i(const float *prev_i,
                                    const float *prev_q,
                                    const float *curr_i,
                                    const float *curr_q,
                                    float *diff_i,
                                    float *diff_q);

#endif
