#ifndef DAB_CORE_FIC_H
#define DAB_CORE_FIC_H

#include <stdint.h>

#include "dab/core/tframe.h"

typedef struct {
    uint32_t frames_seen;
    uint64_t last_frame_start_sample;
    int8_t first_softbit;
    int8_t last_softbit;
} dab_fic_state_t;

int dab_fic_init(dab_fic_state_t *state);
void dab_fic_reset(dab_fic_state_t *state);
int dab_fic_process_frame(dab_fic_state_t *state,
                          uint64_t frame_start_sample,
                          const int8_t *fic_softbits,
                          uint32_t fic_softbit_count);

#endif
