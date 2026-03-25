#include "dab/core/ofdm.h"

#include "test_support.h"

void test_ofdm_extract_active_carriers_mode_i_reorders_fft_bins(void) {
    float fft_i[DAB_MODE_I_FFT_SAMPLES] = {0};
    float fft_q[DAB_MODE_I_FFT_SAMPLES] = {0};
    float active_i[DAB_MODE_I_ACTIVE_CARRIERS] = {0};
    float active_q[DAB_MODE_I_ACTIVE_CARRIERS] = {0};

    fft_i[2048u - 768u] = 11.0f;
    fft_q[2048u - 768u] = -1.0f;
    fft_i[2047u] = 22.0f;
    fft_q[2047u] = -2.0f;
    fft_i[0u] = 33.0f;
    fft_q[0u] = -3.0f;
    fft_i[768u] = 44.0f;
    fft_q[768u] = -4.0f;

    TEST_ASSERT(dab_ofdm_extract_active_carriers_mode_i(fft_i, fft_q, active_i, active_q) == 0);
    TEST_ASSERT(active_i[0u] == 11.0f);
    TEST_ASSERT(active_q[0u] == -1.0f);
    TEST_ASSERT(active_i[767u] == 22.0f);
    TEST_ASSERT(active_q[767u] == -2.0f);
    TEST_ASSERT(active_i[768u] == 33.0f);
    TEST_ASSERT(active_q[768u] == -3.0f);
    TEST_ASSERT(active_i[1536u] == 44.0f);
    TEST_ASSERT(active_q[1536u] == -4.0f);
}

void test_ofdm_diff_demodulate_mode_i_multiplies_by_previous_conjugate(void) {
    float prev_i[DAB_MODE_I_ACTIVE_CARRIERS] = {0};
    float prev_q[DAB_MODE_I_ACTIVE_CARRIERS] = {0};
    float curr_i[DAB_MODE_I_ACTIVE_CARRIERS] = {0};
    float curr_q[DAB_MODE_I_ACTIVE_CARRIERS] = {0};
    float diff_i[DAB_MODE_I_DATA_CARRIERS] = {0};
    float diff_q[DAB_MODE_I_DATA_CARRIERS] = {0};

    prev_i[0] = 1.0f;
    prev_q[0] = 1.0f;
    curr_i[0] = 1.0f;
    curr_q[0] = -1.0f;

    TEST_ASSERT(dab_ofdm_diff_demodulate_mode_i(prev_i, prev_q, curr_i, curr_q, diff_i, diff_q) == 0);
    TEST_ASSERT(diff_i[0] == 0.0f);
    TEST_ASSERT(diff_q[0] == -2.0f);
}
