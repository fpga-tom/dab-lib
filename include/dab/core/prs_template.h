#ifndef DAB_CORE_PRS_TEMPLATE_H
#define DAB_CORE_PRS_TEMPLATE_H

#include <stdint.h>

typedef struct {
    uint32_t fft_samples;
    float *ref_i;
    float *ref_q;
    uint8_t *active_bins;
} dab_prs_template_t;

typedef struct {
    uint16_t bin;
    float re;
    float im;
} dab_prs_template_bin_t;

int dab_prs_template_init(dab_prs_template_t *templ,
                          uint32_t fft_samples,
                          float *ref_i,
                          float *ref_q,
                          uint8_t *active_bins);
void dab_prs_template_clear(dab_prs_template_t *templ);
int dab_prs_template_set_bin(dab_prs_template_t *templ, uint32_t bin, float re, float im);
uint32_t dab_prs_template_active_count(const dab_prs_template_t *templ);
int dab_prs_template_load_bins(dab_prs_template_t *templ,
                               const dab_prs_template_bin_t *bins,
                               uint32_t count);
int dab_prs_template_load_mode_i(dab_prs_template_t *templ);

#endif
