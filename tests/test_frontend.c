#include "dab/core/frontend.h"

#include "test_support.h"

void test_frontend_converts_iq_and_preserves_metadata(void) {
    dab_frontend_t frontend;
    dab_frontend_config_t config;
    const int16_t iq[] = {100, -200, 300, -400, 500, -600};
    dab_sdr_block_view_t src;
    dab_frontend_block_t dst;
    float i_data[3];
    float q_data[3];

    dab_frontend_default_config(&config);
    config.dc_alpha = 0.0f + 0.001f;
    TEST_ASSERT(dab_frontend_init(&frontend, &config) == 0);

    src.iq_interleaved = (int16_t *)iq;
    src.sample_count = 3u;
    src.sample_index = 77u;
    src.timestamp_ns = 12345u;

    dst.i_data = i_data;
    dst.q_data = q_data;

    TEST_ASSERT(dab_frontend_process(&frontend, &src, &dst) == 0);
    TEST_ASSERT(dst.sample_count == 3u);
    TEST_ASSERT(dst.sample_index == 77u);
    TEST_ASSERT(dst.timestamp_ns == 12345u);
    TEST_ASSERT(i_data[0] > 99.0f && i_data[0] < 100.0f);
    TEST_ASSERT(q_data[0] < -199.0f && q_data[0] > -200.0f);
}

void test_frontend_dc_removal_reduces_constant_offset(void) {
    dab_frontend_t frontend;
    dab_frontend_config_t config;
    int16_t iq[2u * 256u];
    dab_sdr_block_view_t src;
    dab_frontend_block_t dst;
    float i_data[256];
    float q_data[256];
    uint32_t i;
    float sum_i = 0.0f;
    float sum_q = 0.0f;

    dab_frontend_default_config(&config);
    config.dc_alpha = 0.1f;
    TEST_ASSERT(dab_frontend_init(&frontend, &config) == 0);

    for (i = 0u; i < 256u; ++i) {
        iq[2u * i] = 1000;
        iq[(2u * i) + 1u] = -500;
    }

    src.iq_interleaved = iq;
    src.sample_count = 256u;
    src.sample_index = 0u;
    src.timestamp_ns = 0u;
    dst.i_data = i_data;
    dst.q_data = q_data;

    TEST_ASSERT(dab_frontend_process(&frontend, &src, &dst) == 0);

    for (i = 0u; i < 256u; ++i) {
        sum_i += i_data[i];
        sum_q += q_data[i];
    }

    TEST_ASSERT(sum_i < 10000.0f && sum_i > -10000.0f);
    TEST_ASSERT(sum_q < 10000.0f && sum_q > -10000.0f);
    TEST_ASSERT(frontend.dc_i > 900.0f);
    TEST_ASSERT(frontend.dc_q < -450.0f);
}
