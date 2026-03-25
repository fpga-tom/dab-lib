#include "dab/core/fic.h"

int dab_fic_init(dab_fic_state_t *state) {
    if (state == 0) {
        return -1;
    }
    dab_fic_reset(state);
    return 0;
}

void dab_fic_reset(dab_fic_state_t *state) {
    if (state == 0) {
        return;
    }
    *state = (dab_fic_state_t){0};
}

int dab_fic_process_frame(dab_fic_state_t *state,
                          uint64_t frame_start_sample,
                          const int8_t *fic_softbits,
                          uint32_t fic_softbit_count) {
    if (state == 0 || fic_softbits == 0 || fic_softbit_count != DAB_TFRAME_FIC_SOFTBITS) {
        return -1;
    }

    state->frames_seen++;
    state->last_frame_start_sample = frame_start_sample;
    state->first_softbit = fic_softbits[0];
    state->last_softbit = fic_softbits[fic_softbit_count - 1u];
    return 0;
}
