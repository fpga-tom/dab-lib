#include "dab/core/sync.h"

#include <stdio.h>

void dab_sync_default_config(dab_sync_config_t *config) {
    if (config == 0) {
        return;
    }

    config->expected_frame_samples = 196608u;
    config->expected_null_samples = 2656u;
    config->acquire_timing_window_samples = 12000u;
    config->track_timing_window_samples = 1024u;
    config->max_consecutive_bad_frames = 4u;
    config->acquire_prs_score_threshold = 0.75f;
    config->track_prs_score_threshold = 0.50f;
    config->cfo_pi_proportional_gain = 0.25f;
    config->cfo_pi_integral_gain = 0.05f;
    config->cfo_pi_integral_limit_hz = 4000.0f;
}

int dab_sync_init(dab_sync_t *sync, const dab_sync_config_t *config) {
    dab_sync_config_t defaults;

    if (sync == 0) {
        return -1;
    }

    dab_sync_default_config(&defaults);
    sync->config = config != 0 ? *config : defaults;
    if (sync->config.expected_frame_samples == 0u ||
        sync->config.expected_null_samples == 0u ||
        sync->config.acquire_timing_window_samples == 0u ||
        sync->config.track_timing_window_samples == 0u ||
        sync->config.max_consecutive_bad_frames == 0u) {
        return -1;
    }
    if (sync->config.acquire_prs_score_threshold <= 0.0f ||
        sync->config.acquire_prs_score_threshold > 1.0f ||
        sync->config.track_prs_score_threshold <= 0.0f ||
        sync->config.track_prs_score_threshold > 1.0f ||
        sync->config.cfo_pi_proportional_gain < 0.0f ||
        sync->config.cfo_pi_integral_gain < 0.0f ||
        sync->config.cfo_pi_integral_limit_hz < 0.0f) {
        return -1;
    }

    dab_sync_reset(sync);
    return 0;
}

void dab_sync_reset(dab_sync_t *sync) {
    if (sync == 0) {
        return;
    }
    sync->state = (dab_sync_state_t){0};
    sync->state.mode = DAB_SYNC_MODE_IDLE;
    sync->state.receiver_snr_db = 0.0f;
    sync->state.receiver_snr_valid = 0;
    sync->state.spectrum_cn_db = 0.0f;
    sync->state.spectrum_cn_valid = 0;
    sync->state.rf_level_dbfs = 0.0f;
    sync->state.rf_level_valid = 0;
}

void dab_sync_on_null_candidate(dab_sync_t *sync, const dab_null_event_t *event) {
    if (sync == 0 || event == 0) {
        return;
    }
    sync->state.frame_start_sample = event->sample_index;
    sync->state.timing_error_samples = (int32_t)event->length - (int32_t)sync->config.expected_null_samples;
    sync->state.mode = DAB_SYNC_MODE_ACQUIRE;
    sync->state.locked = 0;
}

int dab_sync_on_prs_acquire(dab_sync_t *sync,
                            uint64_t prs_peak_sample,
                            float prs_score,
                            float coarse_freq_error_hz,
                            int32_t carrier_offset_bins) {
    if (sync == 0 || sync->state.mode != DAB_SYNC_MODE_ACQUIRE) {
        return -1;
    }

    sync->state.prs_score = prs_score;
    if (prs_score < sync->config.acquire_prs_score_threshold) {
        sync->state.bad_frames++;
        return 0;
    }

    sync->state.frame_start_sample = prs_peak_sample;
    sync->state.coarse_freq_error_hz = coarse_freq_error_hz;
    sync->state.carrier_offset_bins = carrier_offset_bins;
    sync->state.fine_freq_error_hz = 0.0f;
    sync->state.cfo_integrator_hz = coarse_freq_error_hz;
    sync->state.good_frames = 1u;
    sync->state.bad_frames = 0u;
    sync->state.mode = DAB_SYNC_MODE_TRACK;
    sync->state.locked = 1;
    return 1;
}

int dab_sync_on_prs_track(dab_sync_t *sync,
                          uint64_t prs_peak_sample,
                          float prs_score,
                          float measured_freq_error_hz,
                          int32_t carrier_offset_bins) {
    int64_t predicted;
    int64_t error;

    if (sync == 0 || sync->state.mode != DAB_SYNC_MODE_TRACK) {
        return -1;
    }

    sync->state.prs_score = prs_score;
    if (prs_score < sync->config.track_prs_score_threshold) {
        sync->state.bad_frames++;
        if (sync->state.bad_frames >= sync->config.max_consecutive_bad_frames) {
            sync->state.mode = DAB_SYNC_MODE_ACQUIRE;
            sync->state.locked = 0;
            return 0;
        }
        return 1;
    }

    predicted = (int64_t)sync->state.frame_start_sample + (int64_t)sync->config.expected_frame_samples;
    error = (int64_t)prs_peak_sample - predicted;
    sync->state.frame_start_sample = prs_peak_sample;
    sync->state.timing_error_samples = (int32_t)error;
    sync->state.carrier_offset_bins = carrier_offset_bins;
    sync->state.fine_freq_error_hz = measured_freq_error_hz - sync->state.coarse_freq_error_hz;
    sync->state.cfo_integrator_hz += sync->config.cfo_pi_integral_gain * sync->state.fine_freq_error_hz;
    if (sync->state.cfo_integrator_hz > sync->config.cfo_pi_integral_limit_hz) {
        sync->state.cfo_integrator_hz = sync->config.cfo_pi_integral_limit_hz;
    } else if (sync->state.cfo_integrator_hz < -sync->config.cfo_pi_integral_limit_hz) {
        sync->state.cfo_integrator_hz = -sync->config.cfo_pi_integral_limit_hz;
    }
    sync->state.coarse_freq_error_hz =
        sync->state.cfo_integrator_hz +
        (sync->config.cfo_pi_proportional_gain * sync->state.fine_freq_error_hz);
    sync->state.good_frames++;
    sync->state.bad_frames = 0u;
    return 1;
}

uint64_t dab_sync_predict_next_frame_start(const dab_sync_t *sync) {
    if (sync == 0) {
        return 0u;
    }
    return sync->state.frame_start_sample + sync->config.expected_frame_samples;
}
