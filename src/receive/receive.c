#include "dab/receive.h"

#include <string.h>

#include "receive_snapshot_internal.h"

void dab_receive_default_config(dab_receive_config_t *config) {
    if (config == 0) {
        return;
    }

    memset(config, 0, sizeof(*config));
    config->driver = "rtlsdr";
    config->gain_db = 40;
    config->agc_enabled = 1;
    config->android_usb_fd = -1;
    config->use_real_device = 1;
}

void dab_receive_snapshot_build(dab_receive_snapshot_t *snapshot,
                                uint64_t sequence,
                                uint64_t timestamp_ns,
                                uint64_t frequency_hz,
                                uint32_t sample_rate_hz,
                                uint64_t blocks_received,
                                uint32_t dropped_blocks,
                                const dab_sync_t *sync,
                                const dab_fic_state_t *fic,
                                const dab_msc_state_t *msc,
                                const dab_msc_dabplus_debug_t *msc_audio) {
    if (snapshot == 0) {
        return;
    }

    memset(snapshot, 0, sizeof(*snapshot));
    snapshot->sequence = sequence;
    snapshot->timestamp_ns = timestamp_ns;
    snapshot->frequency_hz = frequency_hz;
    snapshot->sample_rate_hz = sample_rate_hz;
    snapshot->blocks_received = blocks_received;
    snapshot->dropped_blocks = dropped_blocks;

    if (sync != 0) {
        const int have_sync_measurement = sync->state.mode != DAB_SYNC_MODE_IDLE;

        snapshot->frame_start_sample = sync->state.frame_start_sample;
        snapshot->locked = sync->state.locked;
        snapshot->good_frames = sync->state.good_frames;
        snapshot->bad_frames = sync->state.bad_frames;
        snapshot->adc_level_valid = sync->state.rf_level_valid;
        snapshot->adc_level_dbfs = sync->state.rf_level_dbfs;
        snapshot->prs_valid = have_sync_measurement;
        snapshot->prs_score = sync->state.prs_score;
        snapshot->cn_valid = sync->state.spectrum_cn_valid;
        snapshot->cn_db = sync->state.spectrum_cn_db;
        snapshot->snr_valid = sync->state.receiver_snr_valid;
        snapshot->snr_db = sync->state.receiver_snr_db;
        snapshot->cfo_valid = have_sync_measurement;
        snapshot->cfo_hz = sync->state.coarse_freq_error_hz;
        snapshot->fine_cfo_hz = sync->state.fine_freq_error_hz;
        snapshot->carrier_offset_bins = sync->state.carrier_offset_bins;
        snapshot->timing_valid = have_sync_measurement;
        snapshot->timing_error_samples = sync->state.timing_error_samples;
    }

    if (fic != 0) {
        snapshot->fic_frames_seen = fic->frames_seen;
        snapshot->fic_blocks_decoded_last_frame = fic->fic_blocks_decoded_last_frame;
        snapshot->fic_valid_fibs_last_frame = fic->valid_fibs_last_frame;
        snapshot->fic_total_valid_fibs = fic->total_valid_fibs;
    }

    if (msc != 0) {
        snapshot->msc_frames_seen = msc->frames_seen;
    }

    if (msc_audio != 0 && msc_audio->valid != 0u) {
        snapshot->msc_audio_valid = 1;
        snapshot->msc_synchronized = msc_audio->synchronized != 0u;
        snapshot->msc_superframe_valid = msc_audio->superframe_valid != 0u;
        snapshot->msc_superframe_generation = msc_audio->generation;
        snapshot->msc_decode_attempts = msc_audio->decode_attempts;
        snapshot->msc_decode_failures = msc_audio->decode_failures;
        snapshot->msc_rs_corrected_last_superframe = msc_audio->rs_corrected_last_superframe;
        snapshot->msc_rs_corrected_total = msc_audio->rs_corrected_total;
        snapshot->msc_last_decode_ok = msc_audio->last_decode_ok != 0u;
        snapshot->msc_runtime_reset_count = msc_audio->runtime_reset_count;
        snapshot->msc_last_reconfig_reason = msc_audio->last_reconfig_reason;
    }
}
