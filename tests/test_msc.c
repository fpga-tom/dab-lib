#include "dab/core/msc.h"

#include "test_support.h"

void test_msc_process_frame_tracks_frame_metadata(void) {
    dab_msc_state_t state;
    int8_t msc[DAB_TFRAME_MSC_SOFTBITS] = {0};

    msc[0] = -7;
    msc[DAB_TFRAME_MSC_SOFTBITS - 1u] = 55;

    TEST_ASSERT(dab_msc_init(&state) == 0);
    TEST_ASSERT(dab_msc_process_frame(&state, 54321u, msc, DAB_TFRAME_MSC_SOFTBITS) == 0);
    TEST_ASSERT(state.frames_seen == 1u);
    TEST_ASSERT(state.last_frame_start_sample == 54321u);
    TEST_ASSERT(state.first_softbit == -7);
    TEST_ASSERT(state.last_softbit == 55);
}
