#include "dab/core/prs_score.h"

#include <math.h>

#include "test_support.h"

void test_prs_score_search_i16_finds_cp_aligned_fft_start(void) {
    dab_prs_score_t score;
    dab_prs_score_config_t config;
    dab_sdr_block_view_t block;
    dab_prs_candidate_t candidate;
    dab_prs_result_t result;
    int16_t iq[2u * 96u] = {0};
    uint32_t i;
    const uint32_t guard = 8u;
    const uint32_t fft = 32u;
    const uint32_t symbol_start = 20u;
    const uint32_t fft_start = symbol_start + guard;
    const uint32_t useful_start = fft_start;
    const uint32_t useful_tail = useful_start + fft - guard;

    dab_prs_score_default_config(&config);
    config.sample_rate = 32000u;
    config.guard_samples = guard;
    config.fft_samples = fft;
    TEST_ASSERT(dab_prs_score_init(&score, &config) == 0);

    for (i = 0u; i < fft; ++i) {
        iq[2u * (useful_start + i)] = (int16_t)(50 + (int)i);
        iq[(2u * (useful_start + i)) + 1u] = (int16_t)(-30 + (int)(i % 5u));
    }
    for (i = 0u; i < guard; ++i) {
        iq[2u * (symbol_start + i)] = iq[2u * (useful_tail + i)];
        iq[(2u * (symbol_start + i)) + 1u] = iq[(2u * (useful_tail + i)) + 1u];
    }

    block.iq_interleaved = iq;
    block.sample_count = 96u;
    block.sample_index = 1000u;
    block.timestamp_ns = 0u;

    candidate.null_start_sample = 900u;
    candidate.prs_symbol_start_sample = 1000u + symbol_start;
    candidate.prs_fft_start_sample = 1000u + fft_start;
    candidate.timing_search_start_offset = 6u;
    candidate.timing_search_length = 12u;

    TEST_ASSERT(dab_prs_score_search_i16(&score, &block, &candidate, &result) == 1);
    TEST_ASSERT(result.valid == 1);
    TEST_ASSERT(result.prs_fft_start_sample == (1000u + fft_start));
    TEST_ASSERT(result.prs_symbol_start_sample == (1000u + symbol_start));
    TEST_ASSERT(result.prs_score > 0.99f);
}

void test_prs_reference_score_f32_distinguishes_match_from_mismatch(void) {
    const float ref_i[4] = {1.0f, 0.0f, -1.0f, 0.0f};
    const float ref_q[4] = {0.0f, 1.0f, 0.0f, -1.0f};
    const float rx_match_i[4] = {2.0f, 0.0f, -2.0f, 0.0f};
    const float rx_match_q[4] = {0.0f, 2.0f, 0.0f, -2.0f};
    const float rx_bad_i[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    const float rx_bad_q[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    dab_prs_reference_score_t good;
    dab_prs_reference_score_t bad;

    TEST_ASSERT(dab_prs_score_reference_f32(rx_match_i, rx_match_q, ref_i, ref_q, 4u, &good) == 0);
    TEST_ASSERT(dab_prs_score_reference_f32(rx_bad_i, rx_bad_q, ref_i, ref_q, 4u, &bad) == 0);

    TEST_ASSERT(good.score > 0.999f);
    TEST_ASSERT(bad.score < 0.30f);
    TEST_ASSERT(good.score > bad.score);
}

void test_prs_reference_template_f32_scores_only_active_bins(void) {
    float ref_i[8];
    float ref_q[8];
    uint8_t active[8];
    float rx_i[8] = {5.0f, 10.0f, -3.0f, 7.0f, 0.0f, 2.0f, -4.0f, 1.0f};
    float rx_q[8] = {0.0f, 2.0f, 1.0f, -1.0f, 0.0f, -2.0f, 3.0f, 4.0f};
    dab_prs_template_t templ;
    dab_prs_reference_score_t score;

    TEST_ASSERT(dab_prs_template_init(&templ, 8u, ref_i, ref_q, active) == 0);
    TEST_ASSERT(dab_prs_template_set_bin(&templ, 1u, 10.0f, 2.0f) == 0);
    TEST_ASSERT(dab_prs_template_set_bin(&templ, 6u, -4.0f, 3.0f) == 0);

    TEST_ASSERT(dab_prs_score_reference_template_f32(rx_i, rx_q, &templ, &score) == 0);
    TEST_ASSERT(score.score > 0.999f);
}

void test_prs_score_symbol_i16_scores_fft_bins_against_template(void) {
    dab_prs_fft_t fft;
    dab_prs_fft_config_t fft_config;
    dab_sdr_block_view_t block;
    dab_prs_template_t templ;
    dab_prs_reference_score_t score;
    int16_t iq[2u * 8u];
    float ref_i[8];
    float ref_q[8];
    uint8_t active[8];
    float fft_i[8];
    float fft_q[8];
    uint32_t n;

    dab_prs_fft_default_config(&fft_config);
    fft_config.fft_samples = 8u;
    TEST_ASSERT(dab_prs_fft_init(&fft, &fft_config) == 0);
    TEST_ASSERT(dab_prs_template_init(&templ, 8u, ref_i, ref_q, active) == 0);
    TEST_ASSERT(dab_prs_template_set_bin(&templ, 1u, 1.0f, 0.0f) == 0);

    for (n = 0u; n < 8u; ++n) {
        const double phase = 2.0 * 3.14159265358979323846 * (double)n / 8.0;
        iq[2u * n] = (int16_t)lrint(100.0 * cos(phase));
        iq[(2u * n) + 1u] = (int16_t)lrint(100.0 * sin(phase));
    }

    block.iq_interleaved = iq;
    block.sample_count = 8u;
    block.sample_index = 200u;
    block.timestamp_ns = 0u;

    TEST_ASSERT(dab_prs_score_symbol_i16(&fft, &block, 200u, &templ, fft_i, fft_q, &score) == 0);
    TEST_ASSERT(score.score > 0.99f);
}

void test_prs_reference_bins_diff_f32_recovers_integer_bin_offset(void) {
    const dab_prs_template_bin_t bins[] = {
        {10u, 1.0f, 0.0f},
        {11u, 0.0f, 1.0f},
        {12u, -1.0f, 0.0f},
        {13u, 0.0f, -1.0f}
    };
    float rx_i[32] = {0};
    float rx_q[32] = {0};
    dab_prs_reference_score_t score;
    uint32_t i;
    const int shift = 2;

    for (i = 0u; i < 4u; ++i) {
        rx_i[bins[i].bin + shift] = bins[i].re;
        rx_q[bins[i].bin + shift] = bins[i].im;
    }

    TEST_ASSERT(dab_prs_score_reference_bins_diff_f32(rx_i, rx_q, bins, 4u, -4, 4, &score) == 0);
    TEST_ASSERT(score.score > 0.99f);
    TEST_ASSERT(score.carrier_offset_bins == shift);
}

void test_prs_score_symbol_i16_mode_i_diff_rejects_missing_window(void) {
    dab_prs_fft_t fft;
    dab_prs_fft_config_t fft_config;
    dab_sdr_block_view_t block;
    dab_prs_reference_score_t score;
    int16_t iq[2u * 16u] = {0};
    float fft_i[8];
    float fft_q[8];

    dab_prs_fft_default_config(&fft_config);
    fft_config.fft_samples = 8u;
    TEST_ASSERT(dab_prs_fft_init(&fft, &fft_config) == 0);

    block.iq_interleaved = iq;
    block.sample_count = 16u;
    block.sample_index = 100u;
    block.timestamp_ns = 0u;

    TEST_ASSERT(dab_prs_score_symbol_i16_mode_i_diff(&fft,
                                                     &block,
                                                     200u,
                                                     fft_i,
                                                     fft_q,
                                                     &score) != 0);
}
