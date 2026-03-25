#include "dab/core/prs_fft.h"

#include <math.h>

#include "test_support.h"

void test_prs_fft_execute_i16_detects_single_bin_tone(void) {
    dab_prs_fft_t fft;
    dab_prs_fft_config_t config;
    dab_sdr_block_view_t block;
    int16_t iq[2u * 8u];
    float out_i[8];
    float out_q[8];
    uint32_t n;
    float mag_bin_1;
    float mag_bin_0;

    dab_prs_fft_default_config(&config);
    config.fft_samples = 8u;
    TEST_ASSERT(dab_prs_fft_init(&fft, &config) == 0);

    for (n = 0u; n < 8u; ++n) {
        const double phase = 2.0 * 3.14159265358979323846 * (double)n / 8.0;
        iq[2u * n] = (int16_t)lrint(100.0 * cos(phase));
        iq[(2u * n) + 1u] = (int16_t)lrint(100.0 * sin(phase));
    }

    block.iq_interleaved = iq;
    block.sample_count = 8u;
    block.sample_index = 100u;
    block.timestamp_ns = 0u;

    TEST_ASSERT(dab_prs_fft_execute_i16(&fft, &block, 100u, out_i, out_q) == 0);

    mag_bin_1 = out_i[1] * out_i[1] + out_q[1] * out_q[1];
    mag_bin_0 = out_i[0] * out_i[0] + out_q[0] * out_q[0];
    TEST_ASSERT(mag_bin_1 > (1000.0f * mag_bin_0));
}

void test_prs_fft_apply_bin_shift_rotates_bins(void) {
    float re[8] = {0};
    float im[8] = {0};

    re[1] = 5.0f;
    im[1] = -2.0f;

    TEST_ASSERT(dab_prs_fft_apply_bin_shift(re, im, 8u, 2) == 0);
    TEST_ASSERT(re[3] == 5.0f);
    TEST_ASSERT(im[3] == -2.0f);
    TEST_ASSERT(re[1] == 0.0f);
    TEST_ASSERT(im[1] == 0.0f);
}
