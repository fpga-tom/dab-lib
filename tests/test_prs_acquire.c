#include "dab/core/prs_acquire.h"

#include "test_support.h"

void test_prs_candidate_from_null_uses_mode_i_boundaries(void) {
    dab_prs_acquire_t acquire;
    dab_prs_acquire_config_t config;
    dab_null_event_t event;
    dab_prs_candidate_t candidate;

    dab_prs_acquire_default_config(&config);
    config.acquire_window_samples = 1200u;
    TEST_ASSERT(dab_prs_acquire_init(&acquire, &config) == 0);

    event.sample_index = 100000u;
    event.length = 2656u;
    event.avg_power = 1.0f;
    event.min_ratio = 0.1f;

    TEST_ASSERT(dab_prs_candidate_from_null(&acquire, &event, &candidate) == 0);
    TEST_ASSERT(candidate.null_start_sample == 100000u);
    TEST_ASSERT(candidate.prs_symbol_start_sample == 102656u);
    TEST_ASSERT(candidate.prs_fft_start_sample == 103160u);
    TEST_ASSERT(candidate.timing_search_start_offset == 600u);
    TEST_ASSERT(candidate.timing_search_length == 1200u);
    TEST_ASSERT(dab_prs_candidate_search_start_sample(&candidate) == 102560u);
    TEST_ASSERT(dab_prs_candidate_search_end_sample(&candidate) == 103760u);
}
