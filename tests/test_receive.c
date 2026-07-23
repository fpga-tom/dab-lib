#include "dab/receive.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "receive_audio_internal.h"
#include "receive_catalog_internal.h"
#include "receive_measurement_internal.h"
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
    TEST_ASSERT(config.service_id == 0u);
    TEST_ASSERT(config.snapshot == 0);
    TEST_ASSERT(config.snapshot_user == 0);
    TEST_ASSERT(config.measurement == 0);
    TEST_ASSERT(config.measurement_user == 0);
    TEST_ASSERT(config.measurement_decimation == 1u);
    TEST_ASSERT(config.stream_watchdog_timeout_ms == 1500u);
    TEST_ASSERT(config.decoder_watchdog_timeout_ms == 1500u);
    TEST_ASSERT(config.catalog == 0);
    TEST_ASSERT(config.catalog_user == 0);
}

void test_receive_service_rejects_incomplete_config_before_opening_device(void) {
    dab_receive_config_t config;

    dab_receive_default_config(&config);
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
    sync.state.residual_freq_error_hz = 0.75f;
    sync.state.carrier_offset_bins = -2;
    sync.state.timing_error_samples = 7;
    sync.state.fft_timing_valid = 1;
    sync.state.fft_timing_offset_samples = 1.25f;
    sync.state.null_confidence_valid = 1;
    sync.state.null_confidence = 0.97f;
    sync.state.cp_correlation_valid = 1;
    sync.state.cp_correlation_peak = 0.91f;
    sync.state.sample_clock_error_valid = 1;
    sync.state.sample_clock_error_ppm = -0.42f;
    sync.state.sample_clock_frame_count = 18u;

    fic.frames_seen = 21u;
    fic.fic_blocks_decoded_last_frame = 3u;
    fic.valid_fibs_last_frame = 9u;
    fic.total_valid_fibs = 123u;
    fic.repo.total_figs = 456u;
    fic.repo.total_fig_parse_errors = 3u;
    msc.frames_seen = 20u;

    msc_audio.valid = 1u;
    msc_audio.synchronized = 1u;
    msc_audio.superframe_valid = 1u;
    msc_audio.generation = 11u;
    msc_audio.decode_attempts = 12u;
    msc_audio.decode_failures = 1u;
    msc_audio.rs_corrected_last_superframe = 2u;
    msc_audio.rs_corrected_total = 17u;
    msc_audio.au_crc_errors_last_superframe = 1u;
    msc_audio.au_crc_errors_total = 4u;
    msc_audio.au_crc_checks_total = 93u;
    msc_audio.ber_error_bits = 21u;
    msc_audio.ber_compared_bits = 100000u;
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
    TEST_ASSERT(snapshot.residual_cfo_hz == 0.75f);
    TEST_ASSERT(snapshot.carrier_offset_bins == -2);
    TEST_ASSERT(snapshot.timing_valid == 1);
    TEST_ASSERT(snapshot.timing_error_samples == 7);
    TEST_ASSERT(snapshot.fft_timing_valid == 1);
    TEST_ASSERT(snapshot.fft_timing_offset_samples == 1.25f);
    TEST_ASSERT(snapshot.null_confidence_valid == 1);
    TEST_ASSERT(snapshot.null_confidence == 0.97f);
    TEST_ASSERT(snapshot.cp_correlation_valid == 1);
    TEST_ASSERT(snapshot.cp_correlation_peak == 0.91f);
    TEST_ASSERT(snapshot.sample_clock_error_valid == 1);
    TEST_ASSERT(snapshot.sample_clock_error_ppm == -0.42f);
    TEST_ASSERT(snapshot.sample_clock_frame_count == 18u);
    TEST_ASSERT(snapshot.fic_frames_seen == 21u);
    TEST_ASSERT(snapshot.fic_blocks_decoded_last_frame == 3u);
    TEST_ASSERT(snapshot.fic_valid_fibs_last_frame == 9u);
    TEST_ASSERT(snapshot.fic_total_valid_fibs == 123u);
    TEST_ASSERT(snapshot.fic_total_figs == 456u);
    TEST_ASSERT(snapshot.fic_total_fig_parse_errors == 3u);
    TEST_ASSERT(snapshot.msc_frames_seen == 20u);
    TEST_ASSERT(snapshot.msc_ber_valid == 1);
    TEST_ASSERT(fabsf(snapshot.msc_ber - 0.00021f) < 0.0000001f);
    TEST_ASSERT(snapshot.msc_ber_error_bits == 21u);
    TEST_ASSERT(snapshot.msc_ber_compared_bits == 100000u);
    TEST_ASSERT(snapshot.msc_audio_valid == 1);
    TEST_ASSERT(snapshot.msc_synchronized == 1);
    TEST_ASSERT(snapshot.msc_superframe_valid == 1);
    TEST_ASSERT(snapshot.msc_superframe_generation == 11u);
    TEST_ASSERT(snapshot.msc_decode_attempts == 12u);
    TEST_ASSERT(snapshot.msc_decode_failures == 1u);
    TEST_ASSERT(snapshot.msc_rs_corrected_last_superframe == 2u);
    TEST_ASSERT(snapshot.msc_rs_corrected_total == 17u);
    TEST_ASSERT(snapshot.msc_au_crc_errors_last_superframe == 1u);
    TEST_ASSERT(snapshot.msc_au_crc_errors_total == 4u);
    TEST_ASSERT(snapshot.msc_au_crc_checks_total == 93u);
    TEST_ASSERT(snapshot.msc_last_decode_ok == 1);
    TEST_ASSERT(snapshot.msc_runtime_reset_count == 3u);
    TEST_ASSERT(snapshot.msc_last_reconfig_reason == 0x42u);
}

void test_receive_audio_format_update_reports_only_real_changes(void) {
    dab_receive_audio_format_state_t state = {0};
    const uint8_t asc_a[] = {0x12u, 0x10u};
    const uint8_t asc_b[] = {0x12u, 0x11u};

    TEST_ASSERT(dab_receive_audio_format_update(&state,
                                                0x5201u,
                                                213360000u,
                                                48000u,
                                                2u,
                                                asc_a,
                                                sizeof(asc_a)) == 1);
    TEST_ASSERT(dab_receive_audio_format_update(&state,
                                                0x5201u,
                                                213360000u,
                                                48000u,
                                                2u,
                                                asc_a,
                                                sizeof(asc_a)) == 0);
    TEST_ASSERT(dab_receive_audio_format_update(&state,
                                                0x5201u,
                                                213360000u,
                                                32000u,
                                                2u,
                                                asc_a,
                                                sizeof(asc_a)) == 1);
    TEST_ASSERT(dab_receive_audio_format_update(&state,
                                                0x5201u,
                                                213360000u,
                                                32000u,
                                                2u,
                                                asc_b,
                                                sizeof(asc_b)) == 1);
    TEST_ASSERT(dab_receive_audio_format_update(&state,
                                                0x5202u,
                                                213360000u,
                                                32000u,
                                                2u,
                                                asc_b,
                                                sizeof(asc_b)) == 1);
}

void test_receive_catalog_copies_fic_ensemble_services_and_components(void) {
    dab_fic_database_t database = {0};
    dab_receive_catalog_t catalog;
    dab_receive_catalog_t same_content;
    dab_fic_service_t *audio_service = &database.services[0];
    dab_fic_service_t *data_service = &database.services[1];
    dab_fic_service_component_t *audio_component = &audio_service->components[0];
    dab_fic_service_component_t *packet_component = &data_service->components[0];

    database.eid = 0xE0C1u;
    database.service_count = 2u;
    database.subchannels[4].valid = 1u;
    database.subchannels[4].subchid = 4u;
    database.subchannels[4].start_cu = 120u;
    database.subchannels[4].size_cu = 90u;
    database.subchannels[4].protection_kind = DAB_FIC_PROTECTION_EEP;
    database.subchannels[4].protection_option = 1u;
    database.subchannels[4].protection_level = 3u;

    audio_service->is_programme = 1u;
    audio_service->sid = 0x1001u;
    audio_service->component_count = 1u;
    audio_component->used = 1u;
    audio_component->tmid = 0u;
    audio_component->sc_type = 63u;
    audio_component->primary = 1u;
    audio_component->has_subchid = 1u;
    audio_component->subchid = 4u;

    data_service->sid = 0x10203040u;
    data_service->component_count = 1u;
    packet_component->used = 1u;
    packet_component->tmid = 3u;
    packet_component->sc_type = 12u;
    packet_component->scid = 0x321u;
    packet_component->has_packet_addr = 1u;
    packet_component->packet_addr = 0x155u;
    packet_component->dg_flag = 1u;

    database.label_count = 3u;
    database.labels[0].used = 1u;
    database.labels[0].scope = DAB_FIC_LABEL_SCOPE_ENSEMBLE;
    database.labels[0].eid = database.eid;
    database.labels[0].charset = 0u;
    strcpy(database.labels[0].text, "TEST ENSEMBLE");
    database.labels[1].used = 1u;
    database.labels[1].scope = DAB_FIC_LABEL_SCOPE_SERVICE;
    database.labels[1].is_programme = 1u;
    database.labels[1].sid = audio_service->sid;
    strcpy(database.labels[1].text, "TEST AUDIO");
    database.labels[2].used = 1u;
    database.labels[2].scope = DAB_FIC_LABEL_SCOPE_SERVICE;
    database.labels[2].sid = data_service->sid;
    strcpy(database.labels[2].text, "TEST DATA");

    dab_receive_catalog_build(&catalog,
                              7u,
                              123456789u,
                              213360000u,
                              &database);

    TEST_ASSERT(catalog.sequence == 7u);
    TEST_ASSERT(catalog.timestamp_ns == 123456789u);
    TEST_ASSERT(catalog.frequency_hz == 213360000u);
    TEST_ASSERT(catalog.ensemble_id == 0xE0C1u);
    TEST_ASSERT(strcmp(catalog.ensemble_label, "TEST ENSEMBLE") == 0);
    TEST_ASSERT(catalog.service_count == 2u);
    TEST_ASSERT(catalog.services[0].service_id == 0x1001u);
    TEST_ASSERT(strcmp(catalog.services[0].label, "TEST AUDIO") == 0);
    TEST_ASSERT(catalog.services[0].component_count == 1u);
    TEST_ASSERT(catalog.services[0].components[0].kind == DAB_RECEIVE_COMPONENT_STREAM_AUDIO);
    TEST_ASSERT(catalog.services[0].components[0].primary == 1u);
    TEST_ASSERT(catalog.services[0].components[0].subchannel_valid == 1u);
    TEST_ASSERT(catalog.services[0].components[0].subchannel_id == 4u);
    TEST_ASSERT(catalog.services[0].components[0].subchannel_start_cu == 120u);
    TEST_ASSERT(catalog.services[0].components[0].subchannel_size_cu == 90u);
    TEST_ASSERT(catalog.services[0].components[0].subchannel_bitrate_kbps == 192u);
    TEST_ASSERT(catalog.services[0].components[0].protection_kind == DAB_FIC_PROTECTION_EEP);
    TEST_ASSERT(catalog.services[0].components[0].protection_option == 1u);
    TEST_ASSERT(catalog.services[0].components[0].protection_level == 3u);
    TEST_ASSERT(catalog.services[1].service_id == 0x10203040u);
    TEST_ASSERT(strcmp(catalog.services[1].label, "TEST DATA") == 0);
    TEST_ASSERT(catalog.services[1].components[0].kind == DAB_RECEIVE_COMPONENT_PACKET_MODE);
    TEST_ASSERT(catalog.services[1].components[0].service_component_id == 0x321u);
    TEST_ASSERT(catalog.services[1].components[0].has_packet_address == 1u);
    TEST_ASSERT(catalog.services[1].components[0].packet_address == 0x155u);

    dab_receive_catalog_build(&same_content,
                              8u,
                              987654321u,
                              213360000u,
                              &database);
    TEST_ASSERT(dab_receive_catalog_content_equal(&catalog, &same_content) != 0);
    same_content.services[0].component_count = 0u;
    TEST_ASSERT(dab_receive_catalog_content_equal(&catalog, &same_content) == 0);
    TEST_ASSERT(dab_receive_catalog_is_regression(&catalog, &same_content) != 0);
    same_content.services[0].component_count = catalog.services[0].component_count;
    same_content.services[0].label[0] = '\0';
    TEST_ASSERT(dab_receive_catalog_is_regression(&catalog, &same_content) != 0);
    same_content.frequency_hz = 220352000u;
    TEST_ASSERT(dab_receive_catalog_is_regression(&catalog, &same_content) == 0);
}

void test_receive_measurement_builds_bounded_signal_payloads(void) {
    dab_receive_measurement_state_t *state =
        (dab_receive_measurement_state_t *)calloc(1u, sizeof(*state));
    dab_receive_measurement_frame_t frame;
    dab_prs_fft_t fft;
    dab_prs_fft_config_t fft_config;
    dab_ofdm_t ofdm;
    dab_ofdm_config_t ofdm_config;
    float *fft_i = (float *)calloc(DAB_MODE_I_FFT_SAMPLES, sizeof(*fft_i));
    float *fft_q = (float *)calloc(DAB_MODE_I_FFT_SAMPLES, sizeof(*fft_q));
    float *physical_i = (float *)calloc(DAB_MODE_I_ACTIVE_CARRIERS, sizeof(*physical_i));
    float *physical_q = (float *)calloc(DAB_MODE_I_ACTIVE_CARRIERS, sizeof(*physical_q));
    float *points_i = (float *)calloc(DAB_MODE_I_DATA_CARRIERS, sizeof(*points_i));
    float *points_q = (float *)calloc(DAB_MODE_I_DATA_CARRIERS, sizeof(*points_q));
    const uint32_t scope_source_count = 220000u;
    const uint64_t scope_source_start = 1000000u;
    const uint64_t scope_null_start = scope_source_start + 10000u;
    const uint64_t scope_prs_fft_start = scope_null_start + 2656u + 504u;
    int16_t *scope_iq = (int16_t *)calloc(2u * scope_source_count, sizeof(*scope_iq));
    dab_sdr_block_view_t scope_samples;
    uint32_t i;

    TEST_ASSERT(state != 0 && fft_i != 0 && fft_q != 0 && physical_i != 0 &&
                physical_q != 0 && points_i != 0 && points_q != 0 && scope_iq != 0);
    dab_prs_fft_default_config(&fft_config);
    TEST_ASSERT(dab_prs_fft_init(&fft, &fft_config) == 0);
    dab_ofdm_default_config(&ofdm_config);
    TEST_ASSERT(dab_ofdm_init(&ofdm, &ofdm_config) == 0);

    for (i = 0u; i < DAB_MODE_I_ACTIVE_CARRIERS; ++i) {
        if (i != DAB_MODE_I_ACTIVE_CARRIER_HALF) {
            if (i == 1u) {
                ofdm.channel_q[i] = -1.0f;
            } else {
                ofdm.channel_i[i] = 1.0f;
            }
            physical_i[i] = (i & 1u) != 0u ? 1.0f : -1.0f;
            physical_q[i] = (i & 2u) != 0u ? 1.0f : -1.0f;
        }
    }
    for (i = 0u; i < DAB_MODE_I_DATA_CARRIERS; ++i) {
        points_i[i] = (i & 1u) != 0u ? 1.0f : -1.0f;
        points_q[i] = (i & 2u) != 0u ? 1.0f : -1.0f;
    }
    fft_i[100u] = 1000000.0f;

    for (i = 0u; i < scope_source_count; ++i) {
        const uint64_t absolute = scope_source_start + i;
        scope_iq[2u * i] = absolute >= scope_null_start &&
                           absolute < scope_null_start + 2656u
            ? 1000
            : 10000;
    }
    scope_samples.iq_interleaved = scope_iq;
    scope_samples.sample_count = scope_source_count;
    scope_samples.sample_index = scope_source_start;
    scope_samples.timestamp_ns = 0u;

    dab_receive_measurement_reset(state);
    TEST_ASSERT(dab_receive_measurement_begin(state, 1u, &fft, fft_i, fft_q, &ofdm) == 0);
    TEST_ASSERT(dab_receive_measurement_capture_null(state,
                                                     &scope_samples,
                                                     scope_prs_fft_start,
                                                     2048000u,
                                                     2656u,
                                                     504u,
                                                     196608u) == 0);
    dab_receive_measurement_add_symbol(state,
                                       physical_i,
                                       physical_q,
                                       points_i,
                                       points_q,
                                       0.0f);
    TEST_ASSERT(dab_receive_measurement_finish(state,
                                               7u,
                                               1234u,
                                               213360000u,
                                               98765u,
                                               2048000u,
                                               &frame) == 0);

    TEST_ASSERT(frame.sequence == 7u);
    TEST_ASSERT(frame.frequency_hz == 213360000u);
    TEST_ASSERT(frame.spectrum_bin_count == DAB_RECEIVE_SPECTRUM_BIN_COUNT);
    TEST_ASSERT(frame.spectrum_dbfs[1124u] > frame.spectrum_dbfs[0u]);
    TEST_ASSERT(frame.constellation_point_count == 64u);
    TEST_ASSERT(fabsf(fabsf(frame.constellation_i[0u]) - 0.7071067f) < 1.0e-4f);
    TEST_ASSERT(frame.cir_bin_count == DAB_RECEIVE_CIR_BIN_COUNT);
    TEST_ASSERT(fabsf(frame.cir_power_db[0u]) < 1.0e-4f);
    TEST_ASSERT(frame.cir_power_db[16u] < -40.0f);
    TEST_ASSERT(frame.channel_response_count == DAB_RECEIVE_CHANNEL_RESPONSE_COUNT);
    TEST_ASSERT(fabsf(frame.channel_magnitude_db[0u]) < 1.0e-4f);
    TEST_ASSERT(fabsf(frame.channel_phase_deg[0u]) < 1.0e-4f);
    TEST_ASSERT(fabsf(frame.channel_phase_deg[1u] - 90.0f) < 1.0e-4f);
    TEST_ASSERT(fabsf(frame.channel_magnitude_db[DAB_RECEIVE_CHANNEL_RESPONSE_COUNT - 1u]) <
                1.0e-4f);
    TEST_ASSERT(frame.carrier_mer_count == DAB_RECEIVE_CARRIER_MER_COUNT);
    TEST_ASSERT(frame.carrier_mer_db[0u] > 50.0f);
    TEST_ASSERT(frame.carrier_mer_db[DAB_RECEIVE_CARRIER_MER_COUNT - 1u] > 50.0f);
    TEST_ASSERT(frame.first_carrier == -768);
    TEST_ASSERT(frame.negative_carrier_count == 768u);
    TEST_ASSERT(frame.null_scope_sample_count == DAB_RECEIVE_NULL_SCOPE_SAMPLE_CAPACITY);
    TEST_ASSERT(frame.null_scope_iq_interleaved != 0);
    TEST_ASSERT(frame.null_scope_power_dbfs != 0);
    TEST_ASSERT(frame.null_scope_start_index > 6000u && frame.null_scope_start_index < 6200u);
    TEST_ASSERT(frame.null_scope_duration_samples > 2580u &&
                frame.null_scope_duration_samples < 2700u);
    TEST_ASSERT(frame.null_scope_depth_db > 19.0f && frame.null_scope_depth_db < 21.0f);
    TEST_ASSERT(frame.null_scope_confidence > 0.98f);
    TEST_ASSERT(frame.null_scope_frame_bin_count == DAB_RECEIVE_NULL_SCOPE_FRAME_BIN_COUNT);
    TEST_ASSERT(frame.null_scope_frame_sample_count == 196608u);

    free(scope_iq);
    free(points_q);
    free(points_i);
    free(physical_q);
    free(physical_i);
    free(fft_q);
    free(fft_i);
    free(state);
}
