#ifndef DAB_CORE_PRS_SCORE_H
#define DAB_CORE_PRS_SCORE_H

#include <stdint.h>

#include "dab/core/prs_acquire.h"
#include "dab/core/prs_fft.h"
#include "dab/core/prs_template.h"
#include "dab/hal/sdr_device.h"

typedef struct {
    uint32_t sample_rate;
    uint32_t guard_samples;
    uint32_t fft_samples;
} dab_prs_score_config_t;

typedef struct {
    float score;
    float correlation_re;
    float correlation_im;
    float signal_energy;
    float reference_energy;
    int32_t carrier_offset_bins;
    float fine_timing_error_samples;
} dab_prs_reference_score_t;

typedef struct {
    float signal_energy;
    float noise_energy;
    float snr_db;
    uint32_t used_carriers;
} dab_prs_snr_result_t;

typedef struct {
    dab_prs_score_config_t config;
} dab_prs_score_t;

void dab_prs_score_default_config(dab_prs_score_config_t *config);
int dab_prs_score_init(dab_prs_score_t *score, const dab_prs_score_config_t *config);
void dab_prs_score_reset(dab_prs_score_t *score);
int dab_prs_score_cp_metric_i16(const dab_prs_score_t *score,
                                const dab_sdr_block_view_t *block,
                                uint64_t fft_start_sample,
                                float *score_out,
                                float *coarse_freq_error_hz_out);

int dab_prs_score_search_i16(const dab_prs_score_t *score,
                             const dab_sdr_block_view_t *block,
                             const dab_prs_candidate_t *candidate,
                             dab_prs_result_t *result);
int dab_prs_score_reference_f32(const float *rx_i,
                                const float *rx_q,
                                const float *ref_i,
                                const float *ref_q,
                                uint32_t count,
                                dab_prs_reference_score_t *result);
int dab_prs_score_reference_template_f32(const float *rx_i,
                                         const float *rx_q,
                                         const dab_prs_template_t *templ,
                                         dab_prs_reference_score_t *result);
int dab_prs_score_reference_bins_diff_f32(const float *rx_i,
                                          const float *rx_q,
                                          const dab_prs_template_bin_t *bins,
                                          uint32_t bin_count,
                                          int32_t min_offset_bins,
                                          int32_t max_offset_bins,
                                          dab_prs_reference_score_t *result);
int dab_prs_score_reference_mode_i_diff_f32(const float *rx_i,
                                            const float *rx_q,
                                            dab_prs_reference_score_t *result);
int dab_prs_score_symbol_i16(const dab_prs_fft_t *fft,
                             const dab_sdr_block_view_t *block,
                             uint64_t fft_start_sample,
                             const dab_prs_template_t *templ,
                             float *fft_i,
                             float *fft_q,
                             dab_prs_reference_score_t *result);
int dab_prs_score_symbol_i16_mode_i_diff(const dab_prs_fft_t *fft,
                                         const dab_sdr_block_view_t *block,
                                         uint64_t fft_start_sample,
                                         float *fft_i,
                                         float *fft_q,
                                         dab_prs_reference_score_t *result);
int dab_prs_score_symbol_i16_mode_i_diff_cfo(const dab_prs_fft_t *fft,
                                             const dab_sdr_block_view_t *block,
                                             uint64_t fft_start_sample,
                                             float cfo_hz,
                                             float *fft_i,
                                             float *fft_q,
                                             dab_prs_reference_score_t *result);
int dab_prs_estimate_snr_f32(const float *rx_i,
                             const float *rx_q,
                             const float *ref_i,
                             const float *ref_q,
                             uint32_t count,
                             dab_prs_snr_result_t *result);
int dab_prs_estimate_equalized_snr_f32(const float *rx_i,
                                       const float *rx_q,
                                       const float *ref_i,
                                       const float *ref_q,
                                       uint32_t count,
                                       dab_prs_snr_result_t *result);
int dab_prs_estimate_spectrum_cn_mode_i_f32(const float *fft_i,
                                            const float *fft_q,
                                            float *cn_db_out);
int dab_prs_estimate_rf_level_dbfs_i16(const dab_sdr_block_view_t *block,
                                       uint64_t start_sample,
                                       uint32_t count,
                                       float *level_dbfs_out);
int dab_prs_score_to_snr_db(float score, float *snr_db_out);

#endif
