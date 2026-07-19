#include "dab/receive.h"

#include <math.h>
#include <string.h>

#include "receive_catalog_internal.h"
#include "receive_measurement_internal.h"
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
    config->measurement_decimation = 1u;
}

static const dab_fic_label_t *receive_catalog_find_label(
    const dab_fic_database_t *database,
    dab_fic_label_scope_t scope,
    uint8_t is_programme,
    uint32_t sid,
    uint16_t eid) {
    uint16_t index;

    if (database == 0) {
        return 0;
    }
    for (index = 0u; index < database->label_count; ++index) {
        const dab_fic_label_t *label = &database->labels[index];
        if (label->used == 0u || label->scope != scope) {
            continue;
        }
        if (scope == DAB_FIC_LABEL_SCOPE_ENSEMBLE && label->eid == eid) {
            return label;
        }
        if (scope == DAB_FIC_LABEL_SCOPE_SERVICE &&
            label->is_programme == is_programme && label->sid == sid) {
            return label;
        }
    }
    return 0;
}

static dab_receive_component_kind_t receive_catalog_component_kind(uint8_t transport_mode_id) {
    switch (transport_mode_id) {
        case 0u:
            return DAB_RECEIVE_COMPONENT_STREAM_AUDIO;
        case 1u:
            return DAB_RECEIVE_COMPONENT_STREAM_DATA;
        case 3u:
            return DAB_RECEIVE_COMPONENT_PACKET_MODE;
        default:
            return DAB_RECEIVE_COMPONENT_UNKNOWN;
    }
}

void dab_receive_catalog_build(dab_receive_catalog_t *catalog,
                               uint64_t sequence,
                               uint64_t timestamp_ns,
                               uint64_t frequency_hz,
                               const dab_fic_database_t *database) {
    const dab_fic_label_t *ensemble_label;
    uint32_t service_index;

    if (catalog == 0) {
        return;
    }
    memset(catalog, 0, sizeof(*catalog));
    catalog->sequence = sequence;
    catalog->timestamp_ns = timestamp_ns;
    catalog->frequency_hz = frequency_hz;
    if (database == 0) {
        return;
    }

    catalog->ensemble_id = database->eid;
    ensemble_label = receive_catalog_find_label(database,
                                                  DAB_FIC_LABEL_SCOPE_ENSEMBLE,
                                                  0u,
                                                  0u,
                                                  database->eid);
    if (ensemble_label != 0) {
        catalog->ensemble_label_charset = ensemble_label->charset;
        memcpy(catalog->ensemble_label,
               ensemble_label->text,
               sizeof(catalog->ensemble_label));
    }

    for (service_index = 0u;
         service_index < database->service_count &&
         catalog->service_count < DAB_RECEIVE_CATALOG_MAX_SERVICES;
         ++service_index) {
        const dab_fic_service_t *source_service = &database->services[service_index];
        dab_receive_service_info_t *service = &catalog->services[catalog->service_count];
        const dab_fic_label_t *service_label;
        uint32_t component_index;

        service->service_id = source_service->sid;
        service->is_programme = source_service->is_programme;
        service->conditional_access_id = source_service->caid;
        service_label = receive_catalog_find_label(database,
                                                     DAB_FIC_LABEL_SCOPE_SERVICE,
                                                     source_service->is_programme,
                                                     source_service->sid,
                                                     0u);
        if (service_label != 0) {
            service->label_charset = service_label->charset;
            memcpy(service->label, service_label->text, sizeof(service->label));
        }

        for (component_index = 0u;
             component_index < source_service->component_count &&
             service->component_count < DAB_RECEIVE_CATALOG_MAX_COMPONENTS_PER_SERVICE;
             ++component_index) {
            const dab_fic_service_component_t *source_component =
                &source_service->components[component_index];
            dab_receive_component_info_t *component;

            if (source_component->used == 0u) {
                continue;
            }
            component = &service->components[service->component_count++];
            component->component_index = (uint8_t)component_index;
            component->kind = receive_catalog_component_kind(source_component->tmid);
            component->transport_mode_id = source_component->tmid;
            component->service_component_type = source_component->sc_type;
            component->primary = source_component->primary;
            component->conditional_access = source_component->ca_flag;
            component->has_subchannel = source_component->has_subchid;
            component->subchannel_id = source_component->subchid;
            component->service_component_id = source_component->scid;
            component->has_packet_address = source_component->has_packet_addr;
            component->packet_address = source_component->packet_addr;
            component->data_group = source_component->dg_flag;

            if (source_component->has_subchid != 0u &&
                source_component->subchid < DAB_FIC_MAX_SUBCHANNELS) {
                const dab_fic_subchannel_t *subchannel =
                    &database->subchannels[source_component->subchid];
                if (subchannel->valid != 0u) {
                    component->subchannel_valid = 1u;
                    component->subchannel_start_cu = subchannel->start_cu;
                    component->subchannel_size_cu = subchannel->size_cu;
                    component->protection_kind = (uint8_t)subchannel->protection_kind;
                    component->protection_table_switch = subchannel->table_switch;
                    component->protection_index = subchannel->protection_index;
                    component->protection_option = subchannel->protection_option;
                    component->protection_level = subchannel->protection_level;
                }
            }
        }
        catalog->service_count++;
    }
}

int dab_receive_catalog_content_equal(const dab_receive_catalog_t *left,
                                      const dab_receive_catalog_t *right) {
    if (left == 0 || right == 0) {
        return 0;
    }
    return left->frequency_hz == right->frequency_hz &&
           left->ensemble_id == right->ensemble_id &&
           left->ensemble_label_charset == right->ensemble_label_charset &&
           memcmp(left->ensemble_label,
                  right->ensemble_label,
                  sizeof(left->ensemble_label)) == 0 &&
           left->service_count == right->service_count &&
           memcmp(left->services, right->services, sizeof(left->services)) == 0;
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

void dab_receive_measurement_reset(dab_receive_measurement_state_t *state) {
    if (state == 0) {
        return;
    }
    memset(state, 0, sizeof(*state));
}

static float receive_power_db(float power, float reference_power) {
    const float ratio = reference_power > 0.0f ? power / reference_power : 0.0f;
    if (ratio <= 1.0e-12f) {
        return -120.0f;
    }
    return 10.0f * log10f(ratio);
}

int dab_receive_measurement_begin(dab_receive_measurement_state_t *state,
                                  uint32_t decimation,
                                  const dab_prs_fft_t *fft,
                                  const float *fft_i,
                                  const float *fft_q,
                                  const dab_ofdm_t *ofdm) {
    const uint32_t fft_count = DAB_RECEIVE_SPECTRUM_BIN_COUNT;
    const float full_scale = (float)fft_count * 32768.0f;
    const float spectrum_reference_power = full_scale * full_scale;
    float cir_peak_power = 0.0f;
    uint32_t i;

    if (state == 0 || fft == 0 || fft_i == 0 || fft_q == 0 || ofdm == 0 ||
        fft->config.fft_samples != fft_count) {
        return -1;
    }

    if (decimation == 0u) {
        decimation = 1u;
    }
    state->frames_seen++;
    state->collecting = ((state->frames_seen - 1u) % decimation) == 0u;
    state->constellation_count = 0u;
    memset(state->carrier_error_sum, 0, sizeof(state->carrier_error_sum));
    memset(state->carrier_symbol_count, 0, sizeof(state->carrier_symbol_count));
    if (!state->collecting) {
        return 0;
    }

    for (i = 0u; i < fft_count; ++i) {
        const uint32_t source = (i + (fft_count / 2u)) % fft_count;
        const float power = (fft_i[source] * fft_i[source]) + (fft_q[source] * fft_q[source]);
        state->spectrum_dbfs[i] = receive_power_db(power, spectrum_reference_power);
        state->cir_i[i] = 0.0f;
        state->cir_q[i] = 0.0f;
    }

    for (i = 0u; i < DAB_MODE_I_ACTIVE_CARRIERS; ++i) {
        uint32_t fft_bin;
        const float equalizer_i = ofdm->channel_i[i];
        const float equalizer_q = ofdm->channel_q[i];
        const float denominator = (equalizer_i * equalizer_i) + (equalizer_q * equalizer_q);

        if (i == DAB_MODE_I_ACTIVE_CARRIER_HALF || denominator <= 1.0e-20f) {
            continue;
        }
        fft_bin = i < DAB_MODE_I_ACTIVE_CARRIER_HALF
            ? DAB_MODE_I_FFT_SAMPLES - DAB_MODE_I_ACTIVE_CARRIER_HALF + i
            : i - DAB_MODE_I_ACTIVE_CARRIER_HALF;
        state->cir_i[fft_bin] = equalizer_i / denominator;
        state->cir_q[fft_bin] = -equalizer_q / denominator;
    }

    if (dab_prs_fft_execute_complex_f32(fft, state->cir_i, state->cir_q, 1) != 0) {
        state->collecting = 0;
        return -1;
    }
    for (i = 0u; i < DAB_RECEIVE_CIR_BIN_COUNT; ++i) {
        const float power = (state->cir_i[i] * state->cir_i[i]) +
                            (state->cir_q[i] * state->cir_q[i]);
        if (power > cir_peak_power) {
            cir_peak_power = power;
        }
    }
    for (i = 0u; i < DAB_RECEIVE_CIR_BIN_COUNT; ++i) {
        const float power = (state->cir_i[i] * state->cir_i[i]) +
                            (state->cir_q[i] * state->cir_q[i]);
        state->cir_power_db[i] = receive_power_db(power, cir_peak_power);
    }
    return 0;
}

void dab_receive_measurement_add_symbol(dab_receive_measurement_state_t *state,
                                        const float *physical_i,
                                        const float *physical_q,
                                        const float *constellation_i,
                                        const float *constellation_q,
                                        float common_phase_error_deg) {
    const float phase = -common_phase_error_deg * (float)(3.14159265358979323846 / 180.0);
    const float rotation_i = cosf(phase);
    const float rotation_q = sinf(phase);
    const float ideal_component = 0.7071067811865475f;
    double power_sum = 0.0;
    float scale;
    uint32_t i;

    if (state == 0 || !state->collecting || physical_i == 0 || physical_q == 0 ||
        constellation_i == 0 || constellation_q == 0) {
        return;
    }

    for (i = 0u; i < DAB_MODE_I_ACTIVE_CARRIERS; ++i) {
        if (i != DAB_MODE_I_ACTIVE_CARRIER_HALF) {
            power_sum += ((double)physical_i[i] * (double)physical_i[i]) +
                         ((double)physical_q[i] * (double)physical_q[i]);
        }
    }
    if (power_sum <= 1.0e-20) {
        return;
    }
    scale = (float)sqrt((double)DAB_RECEIVE_CARRIER_MER_COUNT / power_sum);

    for (i = 0u; i < DAB_MODE_I_ACTIVE_CARRIERS; ++i) {
        uint32_t carrier;
        float rotated_i;
        float rotated_q;
        float normalized_i;
        float normalized_q;
        float ideal_i;
        float ideal_q;
        float error_i;
        float error_q;

        if (i == DAB_MODE_I_ACTIVE_CARRIER_HALF) {
            continue;
        }
        carrier = i < DAB_MODE_I_ACTIVE_CARRIER_HALF ? i : i - 1u;
        rotated_i = (physical_i[i] * rotation_i) - (physical_q[i] * rotation_q);
        rotated_q = (physical_i[i] * rotation_q) + (physical_q[i] * rotation_i);
        normalized_i = rotated_i * scale;
        normalized_q = rotated_q * scale;
        ideal_i = normalized_i >= 0.0f ? ideal_component : -ideal_component;
        ideal_q = normalized_q >= 0.0f ? ideal_component : -ideal_component;
        error_i = normalized_i - ideal_i;
        error_q = normalized_q - ideal_q;
        state->carrier_error_sum[carrier] +=
            ((double)error_i * (double)error_i) + ((double)error_q * (double)error_q);
        state->carrier_symbol_count[carrier]++;
    }

    if (state->constellation_count < DAB_RECEIVE_CONSTELLATION_POINT_CAPACITY) {
        const uint32_t remaining = DAB_RECEIVE_CONSTELLATION_POINT_CAPACITY -
                                   state->constellation_count;
        const uint32_t take = remaining < 64u ? remaining : 64u;
        uint32_t point;

        for (point = 0u; point < take; ++point) {
            const uint32_t source = (point * DAB_MODE_I_DATA_CARRIERS) / take;
            const float source_i = constellation_i[source];
            const float source_q = constellation_q[source];
            const uint32_t destination = state->constellation_count++;
            state->constellation_i[destination] =
                ((source_i * rotation_i) - (source_q * rotation_q)) * scale;
            state->constellation_q[destination] =
                ((source_i * rotation_q) + (source_q * rotation_i)) * scale;
        }
    }
}

int dab_receive_measurement_finish(dab_receive_measurement_state_t *state,
                                   uint64_t sequence,
                                   uint64_t timestamp_ns,
                                   uint64_t frequency_hz,
                                   uint64_t frame_start_sample,
                                   uint32_t sample_rate_hz,
                                   dab_receive_measurement_frame_t *frame) {
    uint32_t i;

    if (state == 0 || frame == 0 || !state->collecting || sample_rate_hz == 0u) {
        return -1;
    }

    for (i = 0u; i < DAB_RECEIVE_CARRIER_MER_COUNT; ++i) {
        const double error = state->carrier_error_sum[i];
        float mer = -20.0f;
        if (state->carrier_symbol_count[i] != 0u) {
            mer = error <= 1.0e-12
                ? 60.0f
                : (float)(10.0 * log10((double)state->carrier_symbol_count[i] / error));
            if (mer < -20.0f) {
                mer = -20.0f;
            } else if (mer > 60.0f) {
                mer = 60.0f;
            }
        }
        state->carrier_mer_db[i] = mer;
    }

    memset(frame, 0, sizeof(*frame));
    frame->sequence = sequence;
    frame->timestamp_ns = timestamp_ns;
    frame->frequency_hz = frequency_hz;
    frame->frame_start_sample = frame_start_sample;
    frame->sample_rate_hz = sample_rate_hz;
    frame->spectrum_dbfs = state->spectrum_dbfs;
    frame->spectrum_bin_count = DAB_RECEIVE_SPECTRUM_BIN_COUNT;
    frame->spectrum_start_hz = -0.5f * (float)sample_rate_hz;
    frame->spectrum_bin_hz = (float)sample_rate_hz / (float)DAB_RECEIVE_SPECTRUM_BIN_COUNT;
    frame->constellation_i = state->constellation_i;
    frame->constellation_q = state->constellation_q;
    frame->constellation_point_count = state->constellation_count;
    frame->cir_power_db = state->cir_power_db;
    frame->cir_bin_count = DAB_RECEIVE_CIR_BIN_COUNT;
    frame->cir_sample_period_ns = 1000000000.0f / (float)sample_rate_hz;
    frame->carrier_mer_db = state->carrier_mer_db;
    frame->carrier_mer_count = DAB_RECEIVE_CARRIER_MER_COUNT;
    frame->first_carrier = -768;
    frame->negative_carrier_count = 768u;
    frame->carrier_spacing_hz = (float)sample_rate_hz /
                                (float)DAB_RECEIVE_SPECTRUM_BIN_COUNT;
    state->collecting = 0;
    return 0;
}
