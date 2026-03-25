#include "dab/core/sync.h"

#include "test_support.h"

void test_sync_acquires_and_tracks(void) {
    dab_sync_t sync;
    dab_sync_config_t config;
    dab_null_event_t event;

    dab_sync_default_config(&config);
    config.expected_frame_samples = 196608u;
    config.expected_null_samples = 2656u;
    config.max_consecutive_bad_frames = 2u;

    TEST_ASSERT(dab_sync_init(&sync, &config) == 0);

    event.sample_index = 100000u;
    event.length = 2600u;
    event.avg_power = 1.0f;
    event.min_ratio = 0.1f;

    dab_sync_on_null_candidate(&sync, &event);
    TEST_ASSERT(sync.state.mode == DAB_SYNC_MODE_ACQUIRE);
    TEST_ASSERT(sync.state.locked == 0);

    TEST_ASSERT(dab_sync_on_prs_acquire(&sync, 101000u, 0.90f, -12, 2) == 1);
    TEST_ASSERT(sync.state.mode == DAB_SYNC_MODE_TRACK);
    TEST_ASSERT(sync.state.locked == 1);
    TEST_ASSERT(sync.state.frame_start_sample == 101000u);
    TEST_ASSERT(sync.state.coarse_freq_error_hz == -12);
    TEST_ASSERT(sync.state.carrier_offset_bins == 2);

    TEST_ASSERT(dab_sync_predict_next_frame_start(&sync) == (101000u + 196608u));

    TEST_ASSERT(dab_sync_on_prs_track(&sync, 297620u, 0.80f, 3.5f, 1) == 1);
    TEST_ASSERT(sync.state.frame_start_sample == 297620u);
    TEST_ASSERT(sync.state.timing_error_samples == 12);
    TEST_ASSERT(sync.state.fine_freq_error_hz == 3.5f);
    TEST_ASSERT(sync.state.carrier_offset_bins == 1);
}

void test_sync_falls_back_to_acquire_after_bad_tracking(void) {
    dab_sync_t sync;
    dab_sync_config_t config;
    dab_null_event_t event;

    dab_sync_default_config(&config);
    config.max_consecutive_bad_frames = 2u;

    TEST_ASSERT(dab_sync_init(&sync, &config) == 0);

    event.sample_index = 50000u;
    event.length = 2656u;
    event.avg_power = 1.0f;
    event.min_ratio = 0.1f;

    dab_sync_on_null_candidate(&sync, &event);
    TEST_ASSERT(dab_sync_on_prs_acquire(&sync, 51000u, 0.95f, 0, 0) == 1);

    TEST_ASSERT(dab_sync_on_prs_track(&sync, 0u, 0.10f, 0.0f, 0) == 1);
    TEST_ASSERT(sync.state.mode == DAB_SYNC_MODE_TRACK);
    TEST_ASSERT(sync.state.locked == 1);

    TEST_ASSERT(dab_sync_on_prs_track(&sync, 0u, 0.10f, 0.0f, 0) == 0);
    TEST_ASSERT(sync.state.mode == DAB_SYNC_MODE_ACQUIRE);
    TEST_ASSERT(sync.state.locked == 0);
}
