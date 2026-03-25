#ifndef DAB_CORE_SYNC_H
#define DAB_CORE_SYNC_H

#include <stdint.h>

#include "dab/core/null_detect.h"

typedef enum {
    DAB_SYNC_MODE_IDLE = 0,
    DAB_SYNC_MODE_ACQUIRE = 1,
    DAB_SYNC_MODE_TRACK = 2
} dab_sync_mode_t;

typedef struct {
    uint32_t expected_frame_samples;
    uint32_t expected_null_samples;
    uint32_t acquire_timing_window_samples;
    uint32_t track_timing_window_samples;
    uint32_t max_consecutive_bad_frames;
    float acquire_prs_score_threshold;
    float track_prs_score_threshold;
} dab_sync_config_t;

typedef struct {
    uint64_t frame_start_sample;
    int32_t timing_error_samples;
    int32_t coarse_freq_error_hz;
    int32_t carrier_offset_bins;
    float fine_freq_error_hz;
    float prs_score;
    uint32_t good_frames;
    uint32_t bad_frames;
    dab_sync_mode_t mode;
    int locked;
} dab_sync_state_t;

typedef struct {
    dab_sync_config_t config;
    dab_sync_state_t state;
} dab_sync_t;

void dab_sync_default_config(dab_sync_config_t *config);
int dab_sync_init(dab_sync_t *sync, const dab_sync_config_t *config);
void dab_sync_reset(dab_sync_t *sync);

void dab_sync_on_null_candidate(dab_sync_t *sync, const dab_null_event_t *event);
int dab_sync_on_prs_acquire(dab_sync_t *sync,
                            uint64_t prs_peak_sample,
                            float prs_score,
                            int32_t coarse_freq_error_hz,
                            int32_t carrier_offset_bins);
int dab_sync_on_prs_track(dab_sync_t *sync,
                          uint64_t prs_peak_sample,
                          float prs_score,
                          float fine_freq_error_hz,
                          int32_t carrier_offset_bins);
uint64_t dab_sync_predict_next_frame_start(const dab_sync_t *sync);

#endif
