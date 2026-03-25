#ifndef DAB_CORE_MSC_H
#define DAB_CORE_MSC_H

#include <stdint.h>

#include "dab/core/tframe.h"

typedef struct {
    uint32_t frames_seen;
    uint64_t last_frame_start_sample;
    int8_t first_softbit;
    int8_t last_softbit;
} dab_msc_state_t;

int dab_msc_init(dab_msc_state_t *state);
void dab_msc_reset(dab_msc_state_t *state);
int dab_msc_process_frame(dab_msc_state_t *state,
                          uint64_t frame_start_sample,
                          const int8_t *msc_softbits,
                          uint32_t msc_softbit_count);

#endif
