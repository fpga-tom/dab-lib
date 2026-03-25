#include "dab/core/prs_acquire.h"

void dab_prs_acquire_default_config(dab_prs_acquire_config_t *config) {
    if (config == 0) {
        return;
    }

    config->null_samples = 2656u;
    config->guard_samples = 504u;
    config->fft_samples = 2048u;
    config->symbol_samples = 2552u;
    config->acquire_window_samples = 1200u;
}

int dab_prs_acquire_init(dab_prs_acquire_t *acquire, const dab_prs_acquire_config_t *config) {
    dab_prs_acquire_config_t defaults;

    if (acquire == 0) {
        return -1;
    }

    dab_prs_acquire_default_config(&defaults);
    acquire->config = config != 0 ? *config : defaults;

    if (acquire->config.null_samples == 0u ||
        acquire->config.guard_samples == 0u ||
        acquire->config.fft_samples == 0u ||
        acquire->config.symbol_samples == 0u ||
        acquire->config.acquire_window_samples == 0u) {
        return -1;
    }
    if (acquire->config.guard_samples + acquire->config.fft_samples != acquire->config.symbol_samples) {
        return -1;
    }

    return 0;
}

void dab_prs_acquire_reset(dab_prs_acquire_t *acquire) {
    (void)acquire;
}

int dab_prs_candidate_from_null(const dab_prs_acquire_t *acquire,
                                const dab_null_event_t *event,
                                dab_prs_candidate_t *candidate) {
    uint64_t prs_symbol_start_sample;
    uint64_t prs_fft_start_sample;
    uint32_t half_window;

    if (acquire == 0 || event == 0 || candidate == 0) {
        return -1;
    }

    prs_symbol_start_sample = event->sample_index + event->length;
    prs_fft_start_sample = prs_symbol_start_sample + acquire->config.guard_samples;
    half_window = acquire->config.acquire_window_samples / 2u;

    candidate->null_start_sample = event->sample_index;
    candidate->prs_symbol_start_sample = prs_symbol_start_sample;
    candidate->prs_fft_start_sample = prs_fft_start_sample;
    candidate->timing_search_start_offset = half_window;
    candidate->timing_search_length = acquire->config.acquire_window_samples;
    return 0;
}

uint64_t dab_prs_candidate_search_start_sample(const dab_prs_candidate_t *candidate) {
    if (candidate == 0) {
        return 0u;
    }
    return candidate->prs_fft_start_sample - candidate->timing_search_start_offset;
}

uint64_t dab_prs_candidate_search_end_sample(const dab_prs_candidate_t *candidate) {
    if (candidate == 0) {
        return 0u;
    }
    return dab_prs_candidate_search_start_sample(candidate) + candidate->timing_search_length;
}
