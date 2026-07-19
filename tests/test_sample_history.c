#include "dab/core/sample_history.h"

#include "test_support.h"

static void fill_iq(int16_t *iq, uint32_t sample_count, int16_t first_value) {
    uint32_t i;

    for (i = 0u; i < sample_count; ++i) {
        iq[2u * i] = (int16_t)(first_value + (int16_t)i);
        iq[(2u * i) + 1u] = (int16_t)(-first_value - (int16_t)i);
    }
}

static void assert_view_values(const dab_sdr_block_view_t *view, int16_t first_value) {
    uint32_t i;

    for (i = 0u; i < view->sample_count; ++i) {
        TEST_ASSERT(view->iq_interleaved[2u * i] == (int16_t)(first_value + (int16_t)i));
        TEST_ASSERT(view->iq_interleaved[(2u * i) + 1u] == (int16_t)(-first_value - (int16_t)i));
    }
}

void test_sample_history_wraps_with_contiguous_view(void) {
    dab_sample_history_t history;
    dab_sdr_block_view_t block;
    dab_sdr_block_view_t view;
    int16_t first[2u * 6u];
    int16_t second[2u * 4u];

    fill_iq(first, 6u, 10);
    fill_iq(second, 4u, 16);
    TEST_ASSERT(dab_sample_history_init(&history, 8u) == 0);

    block = (dab_sdr_block_view_t){first, 6u, 100u, 0u};
    TEST_ASSERT(dab_sample_history_append(&history, &block) == 0);
    block = (dab_sdr_block_view_t){second, 4u, 106u, 0u};
    TEST_ASSERT(dab_sample_history_append(&history, &block) == 0);

    view = dab_sample_history_view(&history);
    TEST_ASSERT(view.sample_index == 102u);
    TEST_ASSERT(view.sample_count == 8u);
    assert_view_values(&view, 12);
    TEST_ASSERT(dab_sample_history_contains_range(&history, 102u, 110u) != 0);
    TEST_ASSERT(dab_sample_history_contains_range(&history, 101u, 110u) == 0);
    TEST_ASSERT(dab_sample_history_contains_range(&history, 102u, 111u) == 0);

    dab_sample_history_destroy(&history);
}

void test_sample_history_resets_on_discontinuity_and_oversized_block(void) {
    dab_sample_history_t history;
    dab_sdr_block_view_t block;
    dab_sdr_block_view_t view;
    int16_t first[2u * 4u];
    int16_t discontinuous[2u * 3u];
    int16_t oversized[2u * 10u];

    fill_iq(first, 4u, 20);
    fill_iq(discontinuous, 3u, 40);
    fill_iq(oversized, 10u, 60);
    TEST_ASSERT(dab_sample_history_init(&history, 8u) == 0);

    block = (dab_sdr_block_view_t){first, 4u, 200u, 0u};
    TEST_ASSERT(dab_sample_history_append(&history, &block) == 0);
    block = (dab_sdr_block_view_t){discontinuous, 3u, 500u, 0u};
    TEST_ASSERT(dab_sample_history_append(&history, &block) == 0);
    view = dab_sample_history_view(&history);
    TEST_ASSERT(view.sample_index == 500u);
    TEST_ASSERT(view.sample_count == 3u);
    assert_view_values(&view, 40);

    block = (dab_sdr_block_view_t){oversized, 10u, 600u, 0u};
    TEST_ASSERT(dab_sample_history_append(&history, &block) == 0);
    view = dab_sample_history_view(&history);
    TEST_ASSERT(view.sample_index == 602u);
    TEST_ASSERT(view.sample_count == 8u);
    assert_view_values(&view, 62);

    dab_sample_history_clear(&history);
    view = dab_sample_history_view(&history);
    TEST_ASSERT(view.iq_interleaved == 0);
    TEST_ASSERT(view.sample_count == 0u);
    dab_sample_history_destroy(&history);
}
