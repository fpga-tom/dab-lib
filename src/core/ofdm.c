#include "dab/core/ofdm.h"

void dab_ofdm_default_config(dab_ofdm_config_t *config) {
    if (config == 0) {
        return;
    }

    config->fft_samples = DAB_MODE_I_FFT_SAMPLES;
    config->active_carriers = DAB_MODE_I_ACTIVE_CARRIERS;
    config->data_carriers = DAB_MODE_I_DATA_CARRIERS;
}

int dab_ofdm_init(dab_ofdm_t *ofdm, const dab_ofdm_config_t *config) {
    dab_ofdm_config_t defaults;

    if (ofdm == 0) {
        return -1;
    }

    dab_ofdm_default_config(&defaults);
    ofdm->config = config != 0 ? *config : defaults;
    if (ofdm->config.fft_samples != DAB_MODE_I_FFT_SAMPLES ||
        ofdm->config.active_carriers != DAB_MODE_I_ACTIVE_CARRIERS ||
        ofdm->config.data_carriers != DAB_MODE_I_DATA_CARRIERS) {
        return -1;
    }

    ofdm->prev_active_i = 0;
    ofdm->prev_active_q = 0;
    ofdm->have_prev_symbol = 0;

    return 0;
}

void dab_ofdm_reset(dab_ofdm_t *ofdm) {
    if (ofdm == 0) {
        return;
    }
    ofdm->have_prev_symbol = 0;
}

int dab_ofdm_extract_active_carriers_mode_i(const float *fft_i,
                                            const float *fft_q,
                                            float *active_i,
                                            float *active_q) {
    uint32_t i;

    if (fft_i == 0 || fft_q == 0 || active_i == 0 || active_q == 0) {
        return -1;
    }

    for (i = 0u; i < DAB_MODE_I_ACTIVE_CARRIER_HALF; ++i) {
        active_i[i] = fft_i[DAB_MODE_I_FFT_SAMPLES - DAB_MODE_I_ACTIVE_CARRIER_HALF + i];
        active_q[i] = fft_q[DAB_MODE_I_FFT_SAMPLES - DAB_MODE_I_ACTIVE_CARRIER_HALF + i];
    }
    for (i = DAB_MODE_I_ACTIVE_CARRIER_HALF; i < DAB_MODE_I_ACTIVE_CARRIERS; ++i) {
        active_i[i] = fft_i[i - DAB_MODE_I_ACTIVE_CARRIER_HALF];
        active_q[i] = fft_q[i - DAB_MODE_I_ACTIVE_CARRIER_HALF];
    }

    return 0;
}

int dab_ofdm_process_mode_i(dab_ofdm_t *ofdm,
                            const float *fft_i,
                            const float *fft_q,
                            uint64_t fft_start_sample,
                            dab_ofdm_symbol_kind_t kind,
                            const dab_ofdm_buffers_t *buffers,
                            dab_ofdm_result_t *result) {
    if (ofdm == 0 || fft_i == 0 || fft_q == 0 || buffers == 0 || result == 0 ||
        buffers->active_i == 0 || buffers->active_q == 0 ||
        buffers->diff_i == 0 || buffers->diff_q == 0) {
        return -1;
    }

    if (dab_ofdm_extract_active_carriers_mode_i(fft_i,
                                                fft_q,
                                                buffers->active_i,
                                                buffers->active_q) != 0) {
        return -1;
    }

    result->fft_start_sample = fft_start_sample;
    result->kind = kind;
    result->have_diff = 0;

    if (kind == DAB_OFDM_SYMBOL_DATA &&
        ofdm->have_prev_symbol &&
        ofdm->prev_active_i != 0 &&
        ofdm->prev_active_q != 0) {
        if (dab_ofdm_diff_demodulate_mode_i(ofdm->prev_active_i,
                                            ofdm->prev_active_q,
                                            buffers->active_i,
                                            buffers->active_q,
                                            buffers->diff_i,
                                            buffers->diff_q) != 0) {
            return -1;
        }
        result->have_diff = 1;
    }

    if (kind == DAB_OFDM_SYMBOL_PRS) {
        ofdm->have_prev_symbol = 0;
        return 0;
    }

    if (ofdm->prev_active_i != 0 && ofdm->prev_active_q != 0) {
        uint32_t i;
        for (i = 0u; i < DAB_MODE_I_ACTIVE_CARRIERS; ++i) {
            ofdm->prev_active_i[i] = buffers->active_i[i];
            ofdm->prev_active_q[i] = buffers->active_q[i];
        }
        ofdm->have_prev_symbol = 1;
    }

    return 0;
}

int dab_ofdm_diff_demodulate_mode_i(const float *prev_i,
                                    const float *prev_q,
                                    const float *curr_i,
                                    const float *curr_q,
                                    float *diff_i,
                                    float *diff_q) {
    uint32_t i;

    if (prev_i == 0 || prev_q == 0 || curr_i == 0 || curr_q == 0 || diff_i == 0 || diff_q == 0) {
        return -1;
    }

    for (i = 0u; i < DAB_MODE_I_DATA_CARRIERS; ++i) {
        const float a_re = curr_i[i];
        const float a_im = curr_q[i];
        const float b_re = prev_i[i];
        const float b_im = prev_q[i];

        diff_i[i] = (a_re * b_re) + (a_im * b_im);
        diff_q[i] = (a_im * b_re) - (a_re * b_im);
    }

    return 0;
}
