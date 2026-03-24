#ifndef DAB_CORE_NULL_DETECT_H
#define DAB_CORE_NULL_DETECT_H

#include <stdint.h>

#include "dab/core/frontend.h"

enum {
    DAB_NULL_DETECT_MAX_WINDOW_SAMPLES = 16384
};

typedef struct {
    float fast_alpha;
    float slow_alpha;
    uint32_t fast_window_samples;
    uint32_t slow_window_samples;
    float enter_ratio_threshold;
    float exit_ratio_threshold;
    uint32_t min_null_samples;
    uint32_t max_null_samples;
    uint32_t startup_ignore_samples;
    uint32_t expected_interval_samples;
    uint32_t interval_tolerance_samples;
    uint32_t max_merge_gap_samples;
} dab_null_detect_config_t;

typedef struct {
    uint64_t sample_index;
    uint32_t length;
    float avg_power;
    float min_ratio;
} dab_null_event_t;

typedef struct {
    dab_null_detect_config_t config;
    float fast_power;
    float slow_power;
    float fast_sum;
    float slow_sum;
    float fast_history[DAB_NULL_DETECT_MAX_WINDOW_SAMPLES];
    float slow_history[DAB_NULL_DETECT_MAX_WINDOW_SAMPLES];
    uint32_t fast_history_pos;
    uint32_t slow_history_pos;
    uint32_t fast_history_count;
    uint32_t slow_history_count;
    uint64_t next_sample_index;
    uint64_t run_start_index;
    float run_power_sum;
    float run_min_ratio;
    uint32_t run_length;
    uint32_t pending_gap_length;
    float pending_gap_power_sum;
    uint32_t block_peak_count;
    uint64_t last_null_sample_index;
    uint32_t has_last_null;
} dab_null_detect_t;

void dab_null_detect_default_config(dab_null_detect_config_t *config);
int dab_null_detect_init(dab_null_detect_t *detector, const dab_null_detect_config_t *config);
void dab_null_detect_reset(dab_null_detect_t *detector);
int dab_null_detect_process(dab_null_detect_t *detector,
                            const dab_frontend_block_t *block,
                            dab_null_event_t *event);

#endif
