#include "dab/receive.h"

#include <string.h>

#include "receive_snapshot_internal.h"
#include "test_support.h"

void test_receive_default_config_uses_rtlsdr_playback_defaults(void) {
    dab_receive_config_t config;

    memset(&config, 0xA5, sizeof(config));
    dab_receive_default_config(&config);

    TEST_ASSERT(config.driver != 0);
    TEST_ASSERT(strcmp(config.driver, "rtlsdr") == 0);
    TEST_ASSERT(config.gain_db == 40);
    TEST_ASSERT(config.agc_enabled == 1);
    TEST_ASSERT(config.android_usb_fd == -1);
    TEST_ASSERT(config.use_real_device == 1);
    TEST_ASSERT(config.snapshot == 0);
    TEST_ASSERT(config.snapshot_user == 0);
}

void test_receive_service_rejects_incomplete_config_before_opening_device(void) {
    dab_receive_config_t config;

    dab_receive_default_config(&config);
    TEST_ASSERT(dab_receive_service(&config, 0, 0) == 2);

    config.frequency_hz = 220352000u;
    TEST_ASSERT(dab_receive_service(&config, 0, 0) == 2);
}

void test_receive_snapshot_maps_receiver_measurements_and_counters(void) {
    dab_receive_snapshot_t snapshot;
    dab_sync_t sync = {0};
    dab_fic_state_t fic = {0};
    dab_msc_state_t msc = {0};
    dab_msc_dabplus_debug_t msc_audio = {0};

    memset(&snapshot, 0xA5, sizeof(snapshot));
    sync.state.mode = DAB_SYNC_MODE_TRACK;
    sync.state.locked = 1;
    sync.state.frame_start_sample = 987654u;
    sync.state.good_frames = 14u;
    sync.state.bad_frames = 2u;
    sync.state.rf_level_valid = 1;
    sync.state.rf_level_dbfs = -12.5f;
    sync.state.prs_score = 0.875f;
    sync.state.spectrum_cn_valid = 1;
    sync.state.spectrum_cn_db = 18.25f;
    sync.state.receiver_snr_valid = 1;
    sync.state.receiver_snr_db = 17.75f;
    sync.state.coarse_freq_error_hz = -123.5f;
    sync.state.fine_freq_error_hz = 4.25f;
    sync.state.carrier_offset_bins = -2;
    sync.state.timing_error_samples = 7;

    fic.frames_seen = 21u;
    fic.fic_blocks_decoded_last_frame = 3u;
    fic.valid_fibs_last_frame = 9u;
    fic.total_valid_fibs = 123u;
    msc.frames_seen = 20u;

    msc_audio.valid = 1u;
    msc_audio.synchronized = 1u;
    msc_audio.superframe_valid = 1u;
    msc_audio.generation = 11u;
    msc_audio.decode_attempts = 12u;
    msc_audio.decode_failures = 1u;
    msc_audio.rs_corrected_last_superframe = 2u;
    msc_audio.rs_corrected_total = 17u;
    msc_audio.last_decode_ok = 1u;
    msc_audio.runtime_reset_count = 3u;
    msc_audio.last_reconfig_reason = 0x42u;

    dab_receive_snapshot_build(&snapshot,
                               5u,
                               123456789u,
                               220352000u,
                               2048000u,
                               77u,
                               4u,
                               &sync,
                               &fic,
                               &msc,
                               &msc_audio);

    TEST_ASSERT(snapshot.sequence == 5u);
    TEST_ASSERT(snapshot.timestamp_ns == 123456789u);
    TEST_ASSERT(snapshot.frequency_hz == 220352000u);
    TEST_ASSERT(snapshot.sample_rate_hz == 2048000u);
    TEST_ASSERT(snapshot.frame_start_sample == 987654u);
    TEST_ASSERT(snapshot.blocks_received == 77u);
    TEST_ASSERT(snapshot.dropped_blocks == 4u);
    TEST_ASSERT(snapshot.locked == 1);
    TEST_ASSERT(snapshot.good_frames == 14u);
    TEST_ASSERT(snapshot.bad_frames == 2u);
    TEST_ASSERT(snapshot.adc_level_valid == 1);
    TEST_ASSERT(snapshot.adc_level_dbfs == -12.5f);
    TEST_ASSERT(snapshot.prs_valid == 1);
    TEST_ASSERT(snapshot.prs_score == 0.875f);
    TEST_ASSERT(snapshot.cn_valid == 1);
    TEST_ASSERT(snapshot.cn_db == 18.25f);
    TEST_ASSERT(snapshot.snr_valid == 1);
    TEST_ASSERT(snapshot.snr_db == 17.75f);
    TEST_ASSERT(snapshot.cfo_valid == 1);
    TEST_ASSERT(snapshot.cfo_hz == -123.5f);
    TEST_ASSERT(snapshot.fine_cfo_hz == 4.25f);
    TEST_ASSERT(snapshot.carrier_offset_bins == -2);
    TEST_ASSERT(snapshot.timing_valid == 1);
    TEST_ASSERT(snapshot.timing_error_samples == 7);
    TEST_ASSERT(snapshot.fic_frames_seen == 21u);
    TEST_ASSERT(snapshot.fic_blocks_decoded_last_frame == 3u);
    TEST_ASSERT(snapshot.fic_valid_fibs_last_frame == 9u);
    TEST_ASSERT(snapshot.fic_total_valid_fibs == 123u);
    TEST_ASSERT(snapshot.msc_frames_seen == 20u);
    TEST_ASSERT(snapshot.msc_audio_valid == 1);
    TEST_ASSERT(snapshot.msc_synchronized == 1);
    TEST_ASSERT(snapshot.msc_superframe_valid == 1);
    TEST_ASSERT(snapshot.msc_superframe_generation == 11u);
    TEST_ASSERT(snapshot.msc_decode_attempts == 12u);
    TEST_ASSERT(snapshot.msc_decode_failures == 1u);
    TEST_ASSERT(snapshot.msc_rs_corrected_last_superframe == 2u);
    TEST_ASSERT(snapshot.msc_rs_corrected_total == 17u);
    TEST_ASSERT(snapshot.msc_last_decode_ok == 1);
    TEST_ASSERT(snapshot.msc_runtime_reset_count == 3u);
    TEST_ASSERT(snapshot.msc_last_reconfig_reason == 0x42u);
}
