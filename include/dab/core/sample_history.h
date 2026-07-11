#ifndef DAB_CORE_SAMPLE_HISTORY_H
#define DAB_CORE_SAMPLE_HISTORY_H

#include <stdint.h>

#include "dab/hal/sdr_device.h"

typedef struct {
    int16_t *iq;
    uint32_t capacity_samples;
    uint32_t sample_count;
    uint32_t write_pos;
    uint64_t start_sample_index;
} dab_sample_history_t;

int dab_sample_history_init(dab_sample_history_t *history, uint32_t capacity_samples);
void dab_sample_history_destroy(dab_sample_history_t *history);
void dab_sample_history_clear(dab_sample_history_t *history);
int dab_sample_history_append(dab_sample_history_t *history, const dab_sdr_block_view_t *block);
dab_sdr_block_view_t dab_sample_history_view(const dab_sample_history_t *history);
int dab_sample_history_contains_range(const dab_sample_history_t *history,
                                      uint64_t start_sample,
                                      uint64_t end_sample);

#endif
