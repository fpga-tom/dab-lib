#include "dab/core/prs_template.h"

#include <string.h>

#include "prs_template_mode_i_table.h"

int dab_prs_template_init(dab_prs_template_t *templ,
                          uint32_t fft_samples,
                          float *ref_i,
                          float *ref_q,
                          uint8_t *active_bins) {
    if (templ == 0 || fft_samples == 0u || ref_i == 0 || ref_q == 0 || active_bins == 0) {
        return -1;
    }

    templ->fft_samples = fft_samples;
    templ->ref_i = ref_i;
    templ->ref_q = ref_q;
    templ->active_bins = active_bins;
    dab_prs_template_clear(templ);
    return 0;
}

void dab_prs_template_clear(dab_prs_template_t *templ) {
    if (templ == 0 || templ->fft_samples == 0u || templ->ref_i == 0 || templ->ref_q == 0 || templ->active_bins == 0) {
        return;
    }

    memset(templ->ref_i, 0, sizeof(*templ->ref_i) * templ->fft_samples);
    memset(templ->ref_q, 0, sizeof(*templ->ref_q) * templ->fft_samples);
    memset(templ->active_bins, 0, sizeof(*templ->active_bins) * templ->fft_samples);
}

int dab_prs_template_set_bin(dab_prs_template_t *templ, uint32_t bin, float re, float im) {
    if (templ == 0 || templ->ref_i == 0 || templ->ref_q == 0 || templ->active_bins == 0) {
        return -1;
    }
    if (bin >= templ->fft_samples) {
        return -1;
    }

    templ->ref_i[bin] = re;
    templ->ref_q[bin] = im;
    templ->active_bins[bin] = 1u;
    return 0;
}

uint32_t dab_prs_template_active_count(const dab_prs_template_t *templ) {
    uint32_t i;
    uint32_t count = 0u;

    if (templ == 0 || templ->active_bins == 0) {
        return 0u;
    }

    for (i = 0u; i < templ->fft_samples; ++i) {
        if (templ->active_bins[i] != 0u) {
            count++;
        }
    }
    return count;
}

int dab_prs_template_load_bins(dab_prs_template_t *templ,
                               const dab_prs_template_bin_t *bins,
                               uint32_t count) {
    uint32_t i;

    if (templ == 0 || bins == 0 || count == 0u) {
        return -1;
    }

    dab_prs_template_clear(templ);
    for (i = 0u; i < count; ++i) {
        if (dab_prs_template_set_bin(templ, bins[i].bin, bins[i].re, bins[i].im) != 0) {
            dab_prs_template_clear(templ);
            return -1;
        }
    }
    return 0;
}

int dab_prs_template_load_mode_i(dab_prs_template_t *templ) {
    if (templ == 0) {
        return -1;
    }
    if (templ->fft_samples != 2048u) {
        return -1;
    }
    if (DAB_PRS_MODE_I_BIN_COUNT == 0u) {
        dab_prs_template_clear(templ);
        return -1;
    }
    return dab_prs_template_load_bins(templ, DAB_PRS_MODE_I_BINS, DAB_PRS_MODE_I_BIN_COUNT);
}
