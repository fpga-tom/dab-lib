#include "dab/core/fic.h"

#include "test_support.h"

void test_fic_process_frame_tracks_frame_metadata(void) {
    dab_fic_state_t state;
    int8_t fic[DAB_TFRAME_FIC_SOFTBITS] = {0};

    fic[0] = 12;
    fic[DAB_TFRAME_FIC_SOFTBITS - 1u] = -34;

    TEST_ASSERT(dab_fic_init(&state) == 0);
    TEST_ASSERT(dab_fic_process_frame(&state, 12345u, fic, DAB_TFRAME_FIC_SOFTBITS) == 0);
    TEST_ASSERT(state.frames_seen == 1u);
    TEST_ASSERT(state.last_frame_start_sample == 12345u);
    TEST_ASSERT(state.first_softbit == 12);
    TEST_ASSERT(state.last_softbit == -34);
}
