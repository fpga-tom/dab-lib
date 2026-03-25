#ifndef DAB_CORE_PRS_ACQUIRE_H
#define DAB_CORE_PRS_ACQUIRE_H

#include <stdint.h>

#include "dab/core/null_detect.h"

typedef struct {
    uint32_t null_samples;
    uint32_t guard_samples;
    uint32_t fft_samples;
    uint32_t symbol_samples;
    uint32_t acquire_window_samples;
} dab_prs_acquire_config_t;

typedef struct {
    uint64_t null_start_sample;
    uint64_t prs_symbol_start_sample;
    uint64_t prs_fft_start_sample;
    uint32_t timing_search_start_offset;
    uint32_t timing_search_length;
} dab_prs_candidate_t;

typedef struct {
    uint64_t prs_peak_sample;
    uint64_t prs_symbol_start_sample;
    uint64_t prs_fft_start_sample;
    float prs_score;
    int32_t coarse_freq_error_hz;
    int valid;
} dab_prs_result_t;

typedef struct {
    dab_prs_acquire_config_t config;
} dab_prs_acquire_t;

void dab_prs_acquire_default_config(dab_prs_acquire_config_t *config);
int dab_prs_acquire_init(dab_prs_acquire_t *acquire, const dab_prs_acquire_config_t *config);
void dab_prs_acquire_reset(dab_prs_acquire_t *acquire);

int dab_prs_candidate_from_null(const dab_prs_acquire_t *acquire,
                                const dab_null_event_t *event,
                                dab_prs_candidate_t *candidate);

uint64_t dab_prs_candidate_search_start_sample(const dab_prs_candidate_t *candidate);
uint64_t dab_prs_candidate_search_end_sample(const dab_prs_candidate_t *candidate);

#endif
