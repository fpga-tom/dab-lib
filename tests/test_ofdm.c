#include "dab/core/ofdm.h"

#include <math.h>
#include <string.h>

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
    float diff_i[DAB_MODE_I_ACTIVE_CARRIERS] = {0};
    float diff_q[DAB_MODE_I_ACTIVE_CARRIERS] = {0};

    prev_i[0] = 1.0f;
    prev_q[0] = 1.0f;
    curr_i[0] = 1.0f;
    curr_q[0] = -1.0f;

    TEST_ASSERT(dab_ofdm_diff_demodulate_mode_i(prev_i, prev_q, curr_i, curr_q, diff_i, diff_q) == 0);
    TEST_ASSERT(diff_i[0] == 0.0f);
    TEST_ASSERT(diff_q[0] == -2.0f);
    TEST_ASSERT(diff_i[DAB_MODE_I_ACTIVE_CARRIER_HALF] == 0.0f);
    TEST_ASSERT(diff_q[DAB_MODE_I_ACTIVE_CARRIER_HALF] == 0.0f);
}

void test_ofdm_process_mode_i_prs_seeds_reference_and_first_data_symbol_produces_diff(void) {
    dab_ofdm_t ofdm;
    dab_ofdm_buffers_t buffers;
    dab_ofdm_result_t result;
    float fft_i[DAB_MODE_I_FFT_SAMPLES] = {0};
    float fft_q[DAB_MODE_I_FFT_SAMPLES] = {0};
    float active_i[DAB_MODE_I_ACTIVE_CARRIERS] = {0};
    float active_q[DAB_MODE_I_ACTIVE_CARRIERS] = {0};
    float prev_i[DAB_MODE_I_ACTIVE_CARRIERS] = {0};
    float prev_q[DAB_MODE_I_ACTIVE_CARRIERS] = {0};
    float diff_i[DAB_MODE_I_ACTIVE_CARRIERS] = {0};
    float diff_q[DAB_MODE_I_ACTIVE_CARRIERS] = {0};
    float deintl_i[DAB_MODE_I_DATA_CARRIERS] = {0};
    float deintl_q[DAB_MODE_I_DATA_CARRIERS] = {0};
    int8_t soft_a[DAB_MODE_I_DATA_CARRIERS] = {0};
    int8_t soft_b[DAB_MODE_I_DATA_CARRIERS] = {0};

    TEST_ASSERT(dab_ofdm_init(&ofdm, 0) == 0);
    ofdm.prev_active_i = prev_i;
    ofdm.prev_active_q = prev_q;

    buffers.active_i = active_i;
    buffers.active_q = active_q;
    buffers.diff_i = diff_i;
    buffers.diff_q = diff_q;
    buffers.deintl_i = deintl_i;
    buffers.deintl_q = deintl_q;
    buffers.soft_a = soft_a;
    buffers.soft_b = soft_b;

    fft_i[DAB_MODE_I_FFT_SAMPLES - DAB_MODE_I_ACTIVE_CARRIER_HALF] = 2.0f * ofdm.prs_ref_i[0];
    fft_q[DAB_MODE_I_FFT_SAMPLES - DAB_MODE_I_ACTIVE_CARRIER_HALF] = 2.0f * ofdm.prs_ref_q[0];
    TEST_ASSERT(dab_ofdm_process_mode_i(&ofdm,
                                        fft_i,
                                        fft_q,
                                        100u,
                                        DAB_OFDM_SYMBOL_PRS,
                                        &buffers,
                                        &result) == 0);
    TEST_ASSERT(result.have_diff == 0);
    TEST_ASSERT(ofdm.have_prev_symbol == 1);
    TEST_ASSERT(fabsf(active_i[0] - ofdm.prs_ref_i[0]) < 1.0e-4f);
    TEST_ASSERT(fabsf(active_q[0] - ofdm.prs_ref_q[0]) < 1.0e-4f);

    fft_i[DAB_MODE_I_FFT_SAMPLES - DAB_MODE_I_ACTIVE_CARRIER_HALF] = -2.0f * ofdm.prs_ref_q[0];
    fft_q[DAB_MODE_I_FFT_SAMPLES - DAB_MODE_I_ACTIVE_CARRIER_HALF] = 2.0f * ofdm.prs_ref_i[0];
    TEST_ASSERT(dab_ofdm_process_mode_i(&ofdm,
                                        fft_i,
                                        fft_q,
                                        200u,
                                        DAB_OFDM_SYMBOL_DATA,
                                        &buffers,
                                        &result) == 0);
    TEST_ASSERT(result.have_diff == 1);
    TEST_ASSERT(result.have_softbits == 1);
    TEST_ASSERT(ofdm.have_prev_symbol == 1);
    TEST_ASSERT(fabsf(diff_i[0] - 0.0f) < 1.0e-3f);
    TEST_ASSERT(fabsf(diff_q[0] - 1.0f) < 1.0e-3f);

    fft_i[DAB_MODE_I_FFT_SAMPLES - DAB_MODE_I_ACTIVE_CARRIER_HALF] = 2.0f * ofdm.prs_ref_i[0];
    fft_q[DAB_MODE_I_FFT_SAMPLES - DAB_MODE_I_ACTIVE_CARRIER_HALF] = -2.0f * ofdm.prs_ref_q[0];
    TEST_ASSERT(dab_ofdm_process_mode_i(&ofdm,
                                        fft_i,
                                        fft_q,
                                        300u,
                                        DAB_OFDM_SYMBOL_DATA,
                                        &buffers,
                                        &result) == 0);
    TEST_ASSERT(result.have_diff == 1);
    TEST_ASSERT(result.have_softbits == 1);
    TEST_ASSERT(fabsf(diff_i[0] - 0.0f) < 1.0e-3f);
    TEST_ASSERT(fabsf(diff_q[0] + 1.0f) < 1.0e-3f);
}

void test_ofdm_qpsk_soft_decode_mode_i_scales_to_signed_softbits(void) {
    float deintl_i[DAB_MODE_I_DATA_CARRIERS];
    float deintl_q[DAB_MODE_I_DATA_CARRIERS];
    int8_t soft_a[DAB_MODE_I_DATA_CARRIERS];
    int8_t soft_b[DAB_MODE_I_DATA_CARRIERS];

    memset(deintl_i, 0, sizeof(deintl_i));
    memset(deintl_q, 0, sizeof(deintl_q));
    deintl_i[0] = 1.0f;
    deintl_q[0] = 0.0f;

    TEST_ASSERT(dab_ofdm_qpsk_soft_decode_mode_i(deintl_i, deintl_q, soft_a, soft_b) == 0);
    TEST_ASSERT(soft_a[0] == -127);
    TEST_ASSERT(soft_b[0] == 0);
}
