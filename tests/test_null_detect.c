#include "dab/core/null_detect.h"

#include "test_support.h"

static void fill_block(dab_frontend_block_t *block,
                       float *i_data,
                       float *q_data,
                       uint64_t sample_index,
                       uint32_t count,
                       float active_level,
                       uint32_t null_start,
                       uint32_t null_end) {
    uint32_t i;

    block->sample_index = sample_index;
    block->timestamp_ns = 0u;
    block->sample_count = count;
    block->i_data = i_data;
    block->q_data = q_data;

    for (i = 0u; i < count; ++i) {
        if (i >= null_start && i < null_end) {
            i_data[i] = 0.0f;
            q_data[i] = 0.0f;
        } else {
            i_data[i] = active_level;
            q_data[i] = 0.0f;
        }
    }
}

void test_null_detect_finds_null_inside_block(void) {
    dab_null_detect_t detector;
    dab_null_detect_config_t config;
    dab_null_event_t event;
    dab_frontend_block_t block;
    float i_data[256];
    float q_data[256];

    dab_null_detect_default_config(&config);
    config.fast_window_samples = 8u;
    config.slow_window_samples = 64u;
    config.enter_ratio_threshold = 0.4f;
    config.exit_ratio_threshold = 0.6f;
    config.min_null_samples = 32u;
    config.max_null_samples = 80u;
    config.max_merge_gap_samples = 4u;
    TEST_ASSERT(dab_null_detect_init(&detector, &config) == 0);

    fill_block(&block, i_data, q_data, 1000u, 256u, 10.0f, 80u, 140u);
    TEST_ASSERT(dab_null_detect_process(&detector, &block, &event) == 1);
    TEST_ASSERT(event.sample_index >= 1084u);
    TEST_ASSERT(event.sample_index <= 1088u);
    TEST_ASSERT(event.length >= 55u);
    TEST_ASSERT(event.length <= 65u);
    TEST_ASSERT(event.avg_power < 1.0f);
    TEST_ASSERT(event.min_ratio < 0.2f);
}

void test_null_detect_tracks_run_across_blocks(void) {
    dab_null_detect_t detector;
    dab_null_detect_config_t config;
    dab_null_event_t event;
    dab_frontend_block_t block_a;
    dab_frontend_block_t block_b;
    float i_a[128];
    float q_a[128];
    float i_b[128];
    float q_b[128];

    dab_null_detect_default_config(&config);
    config.fast_window_samples = 8u;
    config.slow_window_samples = 64u;
    config.enter_ratio_threshold = 0.45f;
    config.exit_ratio_threshold = 0.65f;
    config.min_null_samples = 48u;
    config.max_null_samples = 96u;
    config.max_merge_gap_samples = 4u;
    TEST_ASSERT(dab_null_detect_init(&detector, &config) == 0);

    fill_block(&block_a, i_a, q_a, 0u, 128u, 8.0f, 96u, 128u);
    fill_block(&block_b, i_b, q_b, 128u, 128u, 8.0f, 0u, 40u);

    TEST_ASSERT(dab_null_detect_process(&detector, &block_a, &event) == 0);
    TEST_ASSERT(dab_null_detect_process(&detector, &block_b, &event) == 1);
    TEST_ASSERT(event.sample_index >= 99u);
    TEST_ASSERT(event.sample_index <= 104u);
    TEST_ASSERT(event.length >= 56u);
    TEST_ASSERT(event.length <= 76u);
    TEST_ASSERT(event.min_ratio < 0.3f);
}

void test_null_detect_rejects_startup_and_bad_cadence_runs(void) {
    dab_null_detect_t detector;
    dab_null_detect_config_t config;
    dab_null_event_t event;
    dab_frontend_block_t block_a;
    dab_frontend_block_t block_b;
    dab_frontend_block_t block_c;
    float i_a[256];
    float q_a[256];
    float i_b[256];
    float q_b[256];
    float i_c[256];
    float q_c[256];

    dab_null_detect_default_config(&config);
    config.fast_window_samples = 8u;
    config.slow_window_samples = 64u;
    config.enter_ratio_threshold = 0.4f;
    config.exit_ratio_threshold = 0.6f;
    config.min_null_samples = 32u;
    config.max_null_samples = 80u;
    config.startup_ignore_samples = 300u;
    config.expected_interval_samples = 200u;
    config.interval_tolerance_samples = 20u;
    config.max_merge_gap_samples = 4u;
    TEST_ASSERT(dab_null_detect_init(&detector, &config) == 0);

    fill_block(&block_a, i_a, q_a, 0u, 256u, 10.0f, 80u, 140u);
    fill_block(&block_b, i_b, q_b, 300u, 256u, 10.0f, 80u, 140u);
    fill_block(&block_c, i_c, q_c, 560u, 256u, 10.0f, 80u, 140u);

    TEST_ASSERT(dab_null_detect_process(&detector, &block_a, &event) == 0);
    TEST_ASSERT(dab_null_detect_process(&detector, &block_b, &event) == 1);
    TEST_ASSERT(event.sample_index >= 384u);
    TEST_ASSERT(event.sample_index <= 388u);
    TEST_ASSERT(dab_null_detect_process(&detector, &block_c, &event) == 0);
}

void test_null_detect_merges_short_gap_inside_run(void) {
    dab_null_detect_t detector;
    dab_null_detect_config_t config;
    dab_null_event_t event;
    dab_frontend_block_t block;
    float i_data[256];
    float q_data[256];
    uint32_t i;

    dab_null_detect_default_config(&config);
    config.fast_window_samples = 8u;
    config.slow_window_samples = 64u;
    config.enter_ratio_threshold = 0.45f;
    config.exit_ratio_threshold = 0.7f;
    config.min_null_samples = 40u;
    config.max_null_samples = 100u;
    config.max_merge_gap_samples = 12u;
    TEST_ASSERT(dab_null_detect_init(&detector, &config) == 0);

    block.sample_index = 0u;
    block.timestamp_ns = 0u;
    block.sample_count = 256u;
    block.i_data = i_data;
    block.q_data = q_data;
    for (i = 0u; i < 256u; ++i) {
        i_data[i] = 8.0f;
        q_data[i] = 0.0f;
    }
    for (i = 80u; i < 110u; ++i) {
        i_data[i] = 0.0f;
    }
    for (i = 112u; i < 145u; ++i) {
        i_data[i] = 0.0f;
    }

    TEST_ASSERT(dab_null_detect_process(&detector, &block, &event) == 1);
    TEST_ASSERT(event.sample_index >= 84u);
    TEST_ASSERT(event.sample_index <= 94u);
    TEST_ASSERT(event.length >= 55u);
}
