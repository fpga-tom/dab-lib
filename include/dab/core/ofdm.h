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

typedef enum {
    DAB_OFDM_SYMBOL_PRS = 0,
    DAB_OFDM_SYMBOL_DATA = 1
} dab_ofdm_symbol_kind_t;

typedef struct {
    float *active_i;
    float *active_q;
    float *diff_i;
    float *diff_q;
} dab_ofdm_buffers_t;

typedef struct {
    uint64_t fft_start_sample;
    dab_ofdm_symbol_kind_t kind;
    int have_diff;
} dab_ofdm_result_t;

typedef struct {
    dab_ofdm_config_t config;
    float *prev_active_i;
    float *prev_active_q;
    int have_prev_symbol;
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
int dab_ofdm_process_mode_i(dab_ofdm_t *ofdm,
                            const float *fft_i,
                            const float *fft_q,
                            uint64_t fft_start_sample,
                            dab_ofdm_symbol_kind_t kind,
                            const dab_ofdm_buffers_t *buffers,
                            dab_ofdm_result_t *result);

#endif
