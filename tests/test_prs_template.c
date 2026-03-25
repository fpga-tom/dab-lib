#include "dab/core/prs_template.h"

#include "test_support.h"

void test_prs_template_tracks_active_bins(void) {
    float ref_i[16];
    float ref_q[16];
    uint8_t active[16];
    dab_prs_template_t templ;

    TEST_ASSERT(dab_prs_template_init(&templ, 16u, ref_i, ref_q, active) == 0);
    TEST_ASSERT(dab_prs_template_active_count(&templ) == 0u);

    TEST_ASSERT(dab_prs_template_set_bin(&templ, 3u, 1.0f, 0.0f) == 0);
    TEST_ASSERT(dab_prs_template_set_bin(&templ, 5u, 0.0f, -1.0f) == 0);
    TEST_ASSERT(dab_prs_template_active_count(&templ) == 2u);
    TEST_ASSERT(active[3] == 1u);
    TEST_ASSERT(ref_i[3] == 1.0f);
    TEST_ASSERT(ref_q[5] == -1.0f);
}

void test_prs_template_load_bins_populates_multiple_entries(void) {
    float ref_i[16];
    float ref_q[16];
    uint8_t active[16];
    dab_prs_template_t templ;
    const dab_prs_template_bin_t bins[] = {
        {2u, 1.0f, 0.0f},
        {9u, 0.0f, -1.0f},
        {12u, -0.5f, 0.5f}
    };

    TEST_ASSERT(dab_prs_template_init(&templ, 16u, ref_i, ref_q, active) == 0);
    TEST_ASSERT(dab_prs_template_load_bins(&templ, bins, 3u) == 0);
    TEST_ASSERT(dab_prs_template_active_count(&templ) == 3u);
    TEST_ASSERT(active[2] == 1u);
    TEST_ASSERT(active[9] == 1u);
    TEST_ASSERT(active[12] == 1u);
    TEST_ASSERT(ref_i[12] == -0.5f);
    TEST_ASSERT(ref_q[12] == 0.5f);
}

void test_prs_template_load_mode_i_populates_real_table(void) {
    float ref_i[2048];
    float ref_q[2048];
    uint8_t active[2048];
    dab_prs_template_t templ;

    TEST_ASSERT(dab_prs_template_init(&templ, 2048u, ref_i, ref_q, active) == 0);
    TEST_ASSERT(dab_prs_template_load_mode_i(&templ) == 0);
    TEST_ASSERT(dab_prs_template_active_count(&templ) > 1000u);
    TEST_ASSERT(active[0] == 0u);
    TEST_ASSERT(active[1280] == 1u);
}
