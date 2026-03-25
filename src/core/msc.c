#include "dab/core/msc.h"

int dab_msc_init(dab_msc_state_t *state) {
    if (state == 0) {
        return -1;
    }
    dab_msc_reset(state);
    return 0;
}

void dab_msc_reset(dab_msc_state_t *state) {
    if (state == 0) {
        return;
    }
    *state = (dab_msc_state_t){0};
}

int dab_msc_process_frame(dab_msc_state_t *state,
                          uint64_t frame_start_sample,
                          const int8_t *msc_softbits,
                          uint32_t msc_softbit_count) {
    if (state == 0 || msc_softbits == 0 || msc_softbit_count != DAB_TFRAME_MSC_SOFTBITS) {
        return -1;
    }

    state->frames_seen++;
    state->last_frame_start_sample = frame_start_sample;
    state->first_softbit = msc_softbits[0];
    state->last_softbit = msc_softbits[msc_softbit_count - 1u];
    return 0;
}
