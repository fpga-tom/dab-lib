#include "dab/core/prs_score.h"

#include <math.h>
#include <stdint.h>

#include "dab/core/ofdm.h"
#include "test_support.h"

static uint32_t prs_snr_rng_state = 0x12345678u;

static float prs_snr_uniform_open(void) {
    prs_snr_rng_state = (prs_snr_rng_state * 1664525u) + 1013904223u;
    return ((float)((prs_snr_rng_state >> 8u) + 1u)) / 16777217.0f;
}

static float prs_snr_gaussian(void) {
    const float u1 = prs_snr_uniform_open();
    const float u2 = prs_snr_uniform_open();
    return sqrtf(-2.0f * logf(u1)) * cosf(6.28318530717958647692f * u2);
}

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

void test_prs_score_cp_metric_i16_estimates_positive_cfo_with_positive_sign(void) {
    dab_prs_score_t score;
    dab_prs_score_config_t config;
    dab_sdr_block_view_t block;
    int16_t iq[2u * 96u] = {0};
    uint32_t i;
    float metric = 0.0f;
    float cfo_hz = 0.0f;
    const uint32_t sample_rate = 32000u;
    const uint32_t guard = 8u;
    const uint32_t fft = 32u;
    const uint32_t symbol_start = 20u;
    const uint32_t fft_start = symbol_start + guard;
    const float expected_cfo_hz = 250.0f;

    dab_prs_score_default_config(&config);
    config.sample_rate = sample_rate;
    config.guard_samples = guard;
    config.fft_samples = fft;
    TEST_ASSERT(dab_prs_score_init(&score, &config) == 0);

    for (i = 0u; i < fft; ++i) {
        const double phase = 2.0 * 3.14159265358979323846 * (double)expected_cfo_hz *
                             (double)(fft_start + i) / (double)sample_rate;
        iq[2u * (fft_start + i)] = (int16_t)lrint(120.0 * cos(phase));
        iq[(2u * (fft_start + i)) + 1u] = (int16_t)lrint(120.0 * sin(phase));
    }
    for (i = 0u; i < guard; ++i) {
        const double phase = 2.0 * 3.14159265358979323846 * (double)expected_cfo_hz *
                             (double)(symbol_start + i) / (double)sample_rate;
        iq[2u * (symbol_start + i)] = (int16_t)lrint(120.0 * cos(phase));
        iq[(2u * (symbol_start + i)) + 1u] = (int16_t)lrint(120.0 * sin(phase));
    }

    block.iq_interleaved = iq;
    block.sample_count = 96u;
    block.sample_index = 1000u;
    block.timestamp_ns = 0u;

    TEST_ASSERT(dab_prs_score_cp_metric_i16(&score,
                                            &block,
                                            1000u + fft_start,
                                            &metric,
                                            &cfo_hz) == 0);
    TEST_ASSERT(metric > 0.95f);
    TEST_ASSERT(fabsf(cfo_hz - expected_cfo_hz) <= 1.0e-3f);
}

void test_prs_score_search_i16_matches_brute_force_metrics(void) {
    dab_prs_score_t score;
    dab_prs_score_config_t config;
    dab_sdr_block_view_t block;
    dab_prs_candidate_t candidate;
    dab_prs_result_t result;
    int16_t iq[2u * 256u];
    uint32_t state = 0x12345678u;
    uint32_t i;
    uint64_t sample;
    uint64_t best_sample = 0u;
    float best_score = -1.0f;
    float best_cfo = 0.0f;
    const uint32_t guard = 16u;
    const uint32_t fft = 64u;
    const uint32_t symbol_start = 96u;
    const uint32_t fft_start = symbol_start + guard;

    dab_prs_score_default_config(&config);
    config.sample_rate = 64000u;
    config.guard_samples = guard;
    config.fft_samples = fft;
    TEST_ASSERT(dab_prs_score_init(&score, &config) == 0);

    for (i = 0u; i < 2u * 256u; ++i) {
        state = (state * 1664525u) + 1013904223u;
        iq[i] = (int16_t)((int32_t)((state >> 16u) & 0x1ffu) - 256);
    }
    for (i = 0u; i < guard; ++i) {
        iq[2u * (symbol_start + i)] = iq[2u * (fft_start + fft - guard + i)];
        iq[(2u * (symbol_start + i)) + 1u] = iq[(2u * (fft_start + fft - guard + i)) + 1u];
    }

    block.iq_interleaved = iq;
    block.sample_count = 256u;
    block.sample_index = 5000u;
    block.timestamp_ns = 0u;
    candidate.null_start_sample = 0u;
    candidate.prs_symbol_start_sample = block.sample_index + 60u - guard;
    candidate.prs_fft_start_sample = block.sample_index + 60u;
    candidate.timing_search_start_offset = 60u;
    candidate.timing_search_length = 140u;

    for (sample = dab_prs_candidate_search_start_sample(&candidate);
         sample < dab_prs_candidate_search_end_sample(&candidate);
         ++sample) {
        float candidate_score;
        float candidate_cfo;

        if (dab_prs_score_cp_metric_i16(&score, &block, sample, &candidate_score, &candidate_cfo) == 0 &&
            candidate_score > best_score) {
            best_score = candidate_score;
            best_cfo = candidate_cfo;
            best_sample = sample;
        }
    }

    TEST_ASSERT(dab_prs_score_search_i16(&score, &block, &candidate, &result) == 1);
    TEST_ASSERT(result.prs_fft_start_sample == best_sample);
    TEST_ASSERT(fabsf(result.prs_score - best_score) <= 1.0e-7f);
    TEST_ASSERT(fabsf(result.coarse_freq_error_hz - best_cfo) <= 1.0e-6f);
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

void test_prs_reference_bins_diff_f32_estimates_fine_timing_error(void) {
    const dab_prs_template_bin_t bins[] = {
        {10u, 1.0f, 0.0f},
        {11u, 0.0f, 1.0f},
        {12u, -1.0f, 0.0f},
        {13u, 0.0f, -1.0f}
    };
    float rx_i[2048] = {0};
    float rx_q[2048] = {0};
    dab_prs_reference_score_t score;
    uint32_t i;
    const float timing_error = 3.25f;

    for (i = 0u; i < 4u; ++i) {
        const double phase = -2.0 * 3.14159265358979323846 *
                             ((double)bins[i].bin * (double)timing_error / 2048.0);
        const double rot_re = cos(phase);
        const double rot_im = sin(phase);

        rx_i[bins[i].bin] = (float)(((double)bins[i].re * rot_re) - ((double)bins[i].im * rot_im));
        rx_q[bins[i].bin] = (float)(((double)bins[i].re * rot_im) + ((double)bins[i].im * rot_re));
    }

    TEST_ASSERT(dab_prs_score_reference_bins_diff_f32(rx_i, rx_q, bins, 4u, 0, 0, &score) == 0);
    TEST_ASSERT(score.score > 0.99f);
    TEST_ASSERT(fabsf(score.fine_timing_error_samples - timing_error) < 0.05f);
}

void test_prs_equalized_snr_tracks_awgn(void) {
    static const float expected_snr_db[] = {4.0f, 8.0f, 12.0f};
    dab_ofdm_t ofdm;
    dab_ofdm_config_t config;
    float rx_i[DAB_MODE_I_ACTIVE_CARRIERS];
    float rx_q[DAB_MODE_I_ACTIVE_CARRIERS];
    uint32_t level;

    dab_ofdm_default_config(&config);
    TEST_ASSERT(dab_ofdm_init(&ofdm, &config) == 0);
    prs_snr_rng_state = 0x12345678u;

    for (level = 0u; level < (uint32_t)(sizeof(expected_snr_db) / sizeof(expected_snr_db[0])); ++level) {
        const float snr_linear = powf(10.0f, expected_snr_db[level] / 10.0f);
        const float noise_sigma = sqrtf(0.5f / snr_linear);
        double measured_sum = 0.0;
        uint32_t trial;

        for (trial = 0u; trial < 32u; ++trial) {
            dab_prs_snr_result_t measured;
            uint32_t carrier;

            for (carrier = 0u; carrier < DAB_MODE_I_ACTIVE_CARRIERS; ++carrier) {
                rx_i[carrier] = ofdm.prs_ref_i[carrier] + (noise_sigma * prs_snr_gaussian());
                rx_q[carrier] = ofdm.prs_ref_q[carrier] + (noise_sigma * prs_snr_gaussian());
            }

            TEST_ASSERT(dab_prs_estimate_equalized_snr_f32(rx_i,
                                                           rx_q,
                                                           ofdm.prs_ref_i,
                                                           ofdm.prs_ref_q,
                                                           DAB_MODE_I_ACTIVE_CARRIERS,
                                                           &measured) == 0);
            TEST_ASSERT(measured.used_carriers == DAB_MODE_I_DATA_CARRIERS);
            measured_sum += (double)measured.snr_db;
        }

        TEST_ASSERT(fabs((measured_sum / 32.0) - (double)expected_snr_db[level]) < 0.35);
    }

    dab_ofdm_reset(&ofdm);
}

void test_prs_spectrum_cn_subtracts_noise_floor(void) {
    float fft_i[DAB_MODE_I_FFT_SAMPLES] = {0};
    float fft_q[DAB_MODE_I_FFT_SAMPLES] = {0};
    const float expected_cn_db = 4.0f;
    const float active_amplitude = sqrtf(1.0f + powf(10.0f, expected_cn_db / 10.0f));
    float measured_cn_db = 0.0f;
    uint32_t bin;

    for (bin = 1u; bin <= DAB_MODE_I_ACTIVE_CARRIER_HALF; ++bin) {
        fft_i[bin] = active_amplitude;
    }
    for (bin = DAB_MODE_I_FFT_SAMPLES - DAB_MODE_I_ACTIVE_CARRIER_HALF;
         bin < DAB_MODE_I_FFT_SAMPLES;
         ++bin) {
        fft_i[bin] = active_amplitude;
    }
    for (bin = 772u; bin <= 1020u; ++bin) {
        fft_i[bin] = 1.0f;
    }
    for (bin = 1028u; bin <= 1276u; ++bin) {
        fft_i[bin] = 1.0f;
    }

    fft_i[0] = 100000.0f;
    fft_i[DAB_MODE_I_FFT_SAMPLES / 2u] = 100000.0f;

    TEST_ASSERT(dab_prs_estimate_spectrum_cn_mode_i_f32(fft_i, fft_q, &measured_cn_db) == 0);
    TEST_ASSERT(fabsf(measured_cn_db - expected_cn_db) < 1.0e-4f);
}

void test_prs_rf_level_dbfs_uses_complex_rms_and_removes_dc(void) {
    const int16_t iq[] = {
        123, -456,
        -789, 321,
        17384, 500,
        1000, 16884,
        -15384, 500,
        1000, -15884
    };
    const int16_t clipped_iq[] = {
        INT16_MIN, 0,
        INT16_MAX, 0,
        0, INT16_MIN,
        0, INT16_MAX
    };
    dab_sdr_block_view_t block;
    float measured_dbfs = 0.0f;
    dab_prs_rf_input_metrics_t metrics;
    uint32_t histogram_total_i = 0u;
    uint32_t histogram_total_q = 0u;
    uint32_t bin;

    block.iq_interleaved = (int16_t *)iq;
    block.sample_count = 6u;
    block.sample_index = 100u;
    block.timestamp_ns = 0u;

    TEST_ASSERT(dab_prs_estimate_rf_level_dbfs_i16(&block, 102u, 4u, &measured_dbfs) == 0);
    TEST_ASSERT(fabsf(measured_dbfs - (-6.0205999f)) < 1.0e-4f);
    TEST_ASSERT(dab_prs_measure_rf_input_i16(&block, 102u, 4u, &metrics) == 0);
    TEST_ASSERT(fabsf(metrics.rms_dbfs - (-6.0205999f)) < 1.0e-4f);
    TEST_ASSERT(fabsf(metrics.peak_dbfs - (-6.0205999f)) < 1.0e-4f);
    TEST_ASSERT(fabsf(metrics.crest_factor_db) < 1.0e-4f);
    TEST_ASSERT(metrics.sample_count == 4u);
    TEST_ASSERT(metrics.clipped_sample_count == 0u);
    TEST_ASSERT(fabsf(metrics.clipping_percent) < 1.0e-6f);
    TEST_ASSERT(fabsf(metrics.i_dc - (1000.0f / 32768.0f)) < 1.0e-6f);
    TEST_ASSERT(fabsf(metrics.q_dc - (500.0f / 32768.0f)) < 1.0e-6f);
    TEST_ASSERT(metrics.iq_quality_valid == 1);
    TEST_ASSERT(fabsf(metrics.amplitude_imbalance_db) < 1.0e-4f);
    TEST_ASSERT(fabsf(metrics.phase_imbalance_deg) < 1.0e-4f);
    TEST_ASSERT(fabsf(metrics.iq_correlation) < 1.0e-4f);
    for (bin = 0u; bin < DAB_PRS_RF_INPUT_HISTOGRAM_BINS; ++bin) {
        histogram_total_i += metrics.i_histogram[bin];
        histogram_total_q += metrics.q_histogram[bin];
    }
    TEST_ASSERT(histogram_total_i == 4u);
    TEST_ASSERT(histogram_total_q == 4u);
    TEST_ASSERT(dab_prs_estimate_rf_level_dbfs_i16(&block, 99u, 4u, &measured_dbfs) != 0);
    TEST_ASSERT(dab_prs_estimate_rf_level_dbfs_i16(&block, 104u, 4u, &measured_dbfs) != 0);

    block.iq_interleaved = (int16_t *)clipped_iq;
    block.sample_count = 4u;
    block.sample_index = 0u;
    TEST_ASSERT(dab_prs_measure_rf_input_i16(&block, 0u, 4u, &metrics) == 0);
    TEST_ASSERT(metrics.clipped_sample_count == 4u);
    TEST_ASSERT(fabsf(metrics.clipping_percent - 100.0f) < 1.0e-6f);
}
