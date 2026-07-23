#include "dab/receive.h"

#include <math.h>
#include <string.h>

#include "dab/core/msc.h"
#include "receive_audio_internal.h"
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
    config->stream_watchdog_timeout_ms = 1500u;
    config->decoder_watchdog_timeout_ms = 1500u;
}

int dab_receive_audio_format_update(dab_receive_audio_format_state_t *state,
                                    uint32_t service_id,
                                    uint64_t frequency_hz,
                                    uint32_t sample_rate,
                                    uint32_t channels,
                                    const uint8_t *asc,
                                    uint32_t asc_length) {
    int changed;

    if (state == 0 || asc == 0 || asc_length == 0u ||
        asc_length > DAB_RECEIVE_AUDIO_ASC_CAPACITY) {
        return -1;
    }

    changed = state->valid == 0u ||
              state->service_id != service_id ||
              state->frequency_hz != frequency_hz ||
              state->sample_rate != sample_rate ||
              state->channels != channels ||
              state->asc_length != asc_length ||
              memcmp(state->asc, asc, asc_length) != 0;
    if (!changed) {
        return 0;
    }

    memset(state, 0, sizeof(*state));
    state->valid = 1u;
    state->service_id = service_id;
    state->frequency_hz = frequency_hz;
    state->sample_rate = sample_rate;
    state->channels = channels;
    memcpy(state->asc, asc, asc_length);
    state->asc_length = asc_length;
    return 1;
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
    catalog->fig0_ext0_valid = database->fig0_ext0_valid;
    catalog->change_flag = database->change_flag;
    catalog->alarm_flag = database->alarm_flag;
    catalog->cif_count = database->cif_count;
    for (uint32_t subchannel_index = 0u;
         subchannel_index < DAB_FIC_MAX_SUBCHANNELS;
         ++subchannel_index) {
        if (database->subchannels[subchannel_index].valid != 0u) {
            catalog->subchannel_count++;
        }
    }
    for (uint32_t left = 0u; left < database->service_count; ++left) {
        for (uint32_t right = left + 1u;
             right < database->service_count;
             ++right) {
            if (database->services[left].sid == database->services[right].sid &&
                database->services[left].is_programme ==
                    database->services[right].is_programme) {
                catalog->duplicate_service_id_count++;
            }
        }
    }
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
            catalog->service_component_count++;

            if (source_component->has_subchid != 0u &&
                source_component->subchid < DAB_FIC_MAX_SUBCHANNELS) {
                const dab_fic_subchannel_t *subchannel =
                    &database->subchannels[source_component->subchid];
                if (subchannel->valid != 0u) {
                    dab_msc_subchannel_t resolved_subchannel;
                    component->subchannel_valid = 1u;
                    component->subchannel_start_cu = subchannel->start_cu;
                    component->subchannel_size_cu = subchannel->size_cu;
                    component->protection_kind = (uint8_t)subchannel->protection_kind;
                    component->protection_table_switch = subchannel->table_switch;
                    component->protection_index = subchannel->protection_index;
                    component->protection_option = subchannel->protection_option;
                    component->protection_level = subchannel->protection_level;
                    if (dab_msc_resolve_subchannel(subchannel, &resolved_subchannel) == 0) {
                        component->subchannel_bitrate_kbps =
                            resolved_subchannel.bitrate_kbps;
                    }
                } else {
                    catalog->missing_subchannel_reference_count++;
                }
            } else if (source_component->has_subchid != 0u) {
                catalog->missing_subchannel_reference_count++;
            }
        }
        catalog->service_count++;
    }
    catalog->database_consistent =
        catalog->missing_subchannel_reference_count == 0u &&
        catalog->duplicate_service_id_count == 0u;
}

int dab_receive_catalog_content_equal(const dab_receive_catalog_t *left,
                                      const dab_receive_catalog_t *right) {
    if (left == 0 || right == 0) {
        return 0;
    }
    return left->frequency_hz == right->frequency_hz &&
           left->ensemble_id == right->ensemble_id &&
           left->fig0_ext0_valid == right->fig0_ext0_valid &&
           left->change_flag == right->change_flag &&
           left->alarm_flag == right->alarm_flag &&
           left->ensemble_label_charset == right->ensemble_label_charset &&
           memcmp(left->ensemble_label,
                  right->ensemble_label,
                  sizeof(left->ensemble_label)) == 0 &&
           left->service_count == right->service_count &&
           left->service_component_count == right->service_component_count &&
           left->subchannel_count == right->subchannel_count &&
           left->missing_subchannel_reference_count ==
               right->missing_subchannel_reference_count &&
           left->duplicate_service_id_count ==
               right->duplicate_service_id_count &&
           left->database_consistent == right->database_consistent &&
           memcmp(left->services, right->services, sizeof(left->services)) == 0;
}

static const dab_receive_service_info_t *receive_catalog_find_service(
    const dab_receive_catalog_t *catalog,
    uint32_t service_id,
    uint8_t is_programme) {
    uint32_t index;

    if (catalog == 0) {
        return 0;
    }
    for (index = 0u; index < catalog->service_count; ++index) {
        const dab_receive_service_info_t *service = &catalog->services[index];
        if (service->service_id == service_id && service->is_programme == is_programme) {
            return service;
        }
    }
    return 0;
}

int dab_receive_catalog_is_regression(const dab_receive_catalog_t *current,
                                      const dab_receive_catalog_t *candidate) {
    uint32_t index;

    if (current == 0 || candidate == 0 || current->ensemble_id == 0u ||
        current->frequency_hz != candidate->frequency_hz ||
        current->ensemble_id != candidate->ensemble_id) {
        return 0;
    }
    if (current->ensemble_label[0] != '\0' && candidate->ensemble_label[0] == '\0') {
        return 1;
    }
    if (candidate->service_count < current->service_count) {
        return 1;
    }

    for (index = 0u; index < current->service_count; ++index) {
        const dab_receive_service_info_t *old_service = &current->services[index];
        const dab_receive_service_info_t *new_service = receive_catalog_find_service(
            candidate,
            old_service->service_id,
            old_service->is_programme);

        if (new_service == 0 || new_service->component_count < old_service->component_count) {
            return 1;
        }
        if (old_service->label[0] != '\0' && new_service->label[0] == '\0') {
            return 1;
        }
    }
    return 0;
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
        snapshot->residual_cfo_hz = sync->state.residual_freq_error_hz;
        snapshot->carrier_offset_bins = sync->state.carrier_offset_bins;
        snapshot->timing_valid = have_sync_measurement;
        snapshot->timing_error_samples = sync->state.timing_error_samples;
        snapshot->fft_timing_valid = sync->state.fft_timing_valid;
        snapshot->fft_timing_offset_samples = sync->state.fft_timing_offset_samples;
        snapshot->null_confidence_valid = sync->state.null_confidence_valid;
        snapshot->null_confidence = sync->state.null_confidence;
        snapshot->cp_correlation_valid = sync->state.cp_correlation_valid;
        snapshot->cp_correlation_peak = sync->state.cp_correlation_peak;
        snapshot->sample_clock_error_valid = sync->state.sample_clock_error_valid;
        snapshot->sample_clock_error_ppm = sync->state.sample_clock_error_ppm;
        snapshot->sample_clock_frame_count = sync->state.sample_clock_frame_count;
    }

    if (fic != 0) {
        snapshot->fic_frames_seen = fic->frames_seen;
        snapshot->fic_blocks_decoded_last_frame = fic->fic_blocks_decoded_last_frame;
        snapshot->fic_valid_fibs_last_frame = fic->valid_fibs_last_frame;
        snapshot->fic_total_valid_fibs = fic->total_valid_fibs;
        snapshot->fic_change_flag = fic->repo.current.change_flag;
        snapshot->fic_alarm_flag = fic->repo.current.alarm_flag;
        snapshot->fic_cif_count = fic->repo.current.cif_count;
        snapshot->fic_frame_start_sample = fic->last_frame_start_sample;
        for (uint32_t fib_index = 0u;
             fib_index < DAB_RECEIVE_FIC_FIB_COUNT;
             ++fib_index) {
            if (fic->fib_crc_ok[fib_index] != 0u) {
                snapshot->fic_crc_valid_mask |= (uint16_t)(1u << fib_index);
            }
            memcpy(snapshot->fic_decoded_fibs[fib_index],
                   fic->decoded_fibs[fib_index],
                   DAB_RECEIVE_FIC_FIB_BYTES);
        }
        snapshot->fic_pre_viterbi_ber_valid =
            fic->pre_viterbi_compared_bits_total != 0u;
        snapshot->fic_pre_viterbi_error_bits =
            fic->pre_viterbi_error_bits_total;
        snapshot->fic_pre_viterbi_compared_bits =
            fic->pre_viterbi_compared_bits_total;
        snapshot->fic_pre_viterbi_ber =
            fic->pre_viterbi_compared_bits_total != 0u
                ? (float)((double)fic->pre_viterbi_error_bits_total /
                          (double)fic->pre_viterbi_compared_bits_total)
                : 0.0f;
        snapshot->fic_viterbi_quality_percent =
            fic->viterbi_quality_last_frame;
        for (uint32_t block_index = 0u;
             block_index < DAB_RECEIVE_FIC_BLOCK_COUNT;
             ++block_index) {
            const dab_fic_debug_block_t *block =
                &fic->block_info_last_frame[block_index];
            snapshot->fic_block_pre_viterbi_ber[block_index] =
                block->pre_viterbi_compared_bits != 0u
                    ? (float)((double)block->pre_viterbi_error_bits /
                              (double)block->pre_viterbi_compared_bits)
                    : 0.0f;
            snapshot->fic_block_viterbi_quality_percent[block_index] =
                block->viterbi_quality_percent;
        }
        snapshot->fic_total_figs = fic->repo.total_figs;
        snapshot->fic_total_fig_parse_errors = fic->repo.total_fig_parse_errors;
        snapshot->fic_fig_stat_count =
            fic->repo.fig_stat_count < DAB_RECEIVE_FIC_FIG_STAT_CAPACITY
                ? fic->repo.fig_stat_count
                : DAB_RECEIVE_FIC_FIG_STAT_CAPACITY;
        for (uint32_t fig_index = 0u;
             fig_index < snapshot->fic_fig_stat_count;
             ++fig_index) {
            const dab_fic_fig_stat_t *source = &fic->repo.fig_stats[fig_index];
            dab_receive_fic_fig_stat_t *target =
                &snapshot->fic_fig_stats[fig_index];

            target->used = source->used;
            target->type = source->type;
            target->extension = source->ext;
            target->supported = source->supported;
            target->total_count = source->total_count;
            target->change_count = source->change_count;
            target->malformed_count = source->malformed_count;
            target->last_seen_frame = source->last_seen_frame;
            target->last_change_frame = source->last_change_frame;
            target->raw_length = source->raw_length;
            target->previous_raw_length = source->previous_raw_length;
            memcpy(target->raw, source->raw, sizeof(target->raw));
            memcpy(target->previous_raw,
                   source->previous_raw,
                   sizeof(target->previous_raw));
        }
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
        snapshot->msc_ber_valid = msc_audio->ber_compared_bits != 0u;
        snapshot->msc_ber_error_bits = msc_audio->ber_error_bits;
        snapshot->msc_ber_compared_bits = msc_audio->ber_compared_bits;
        snapshot->msc_ber = msc_audio->ber_compared_bits != 0u
            ? (float)((double)msc_audio->ber_error_bits /
                      (double)msc_audio->ber_compared_bits)
            : 0.0f;
        snapshot->msc_rs_corrected_last_superframe = msc_audio->rs_corrected_last_superframe;
        snapshot->msc_rs_corrected_total = msc_audio->rs_corrected_total;
        snapshot->msc_au_crc_errors_last_superframe =
            msc_audio->au_crc_errors_last_superframe;
        snapshot->msc_au_crc_errors_total = msc_audio->au_crc_errors_total;
        snapshot->msc_au_crc_checks_total = msc_audio->au_crc_checks_total;
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

static int receive_iq_mean_power(const dab_sdr_block_view_t *samples,
                                 uint64_t start_sample,
                                 uint32_t count,
                                 double *power_out) {
    uint64_t offset;
    double power = 0.0;
    uint32_t index;

    if (samples == 0 || samples->iq_interleaved == 0 || power_out == 0 || count == 0u ||
        start_sample < samples->sample_index) {
        return -1;
    }
    offset = start_sample - samples->sample_index;
    if (offset > samples->sample_count || count > samples->sample_count - offset) {
        return -1;
    }
    for (index = 0u; index < count; ++index) {
        const double i = samples->iq_interleaved[2u * (offset + index)];
        const double q = samples->iq_interleaved[(2u * (offset + index)) + 1u];
        power += (i * i) + (q * q);
    }
    *power_out = power / (double)count;
    return 0;
}

static float receive_iq_power_dbfs(double power) {
    const double reference = 32768.0 * 32768.0;
    return power > reference * 1.0e-12
        ? (float)(10.0 * log10(power / reference))
        : -120.0f;
}

int dab_receive_measurement_capture_null(dab_receive_measurement_state_t *state,
                                         const dab_sdr_block_view_t *samples,
                                         uint64_t prs_fft_start_sample,
                                         uint32_t sample_rate_hz,
                                         uint32_t null_samples,
                                         uint32_t guard_samples,
                                         uint32_t frame_samples) {
    const uint32_t focus_pre_samples = 6144u;
    const uint32_t edge_search_samples = 512u;
    const uint32_t edge_window_samples = 32u;
    const uint32_t level_window_samples = 512u;
    uint64_t expected_end;
    uint64_t expected_start;
    uint64_t focus_start;
    uint64_t focus_end;
    uint64_t detected_start;
    uint64_t detected_end;
    double before_power;
    double after_power;
    double signal_power;
    double null_power;
    double threshold_power;
    uint32_t index;

    if (state == 0 || !state->collecting || samples == 0 || samples->iq_interleaved == 0 ||
        sample_rate_hz == 0u || null_samples < 256u || guard_samples == 0u ||
        prs_fft_start_sample < (uint64_t)guard_samples + null_samples) {
        return -1;
    }

    expected_end = prs_fft_start_sample - guard_samples;
    expected_start = expected_end - null_samples;
    if (expected_start < focus_pre_samples) {
        return -1;
    }
    focus_start = expected_start - focus_pre_samples;
    focus_end = focus_start + DAB_RECEIVE_NULL_SCOPE_SAMPLE_CAPACITY;
    if (focus_end < focus_start || focus_start < samples->sample_index ||
        focus_end > samples->sample_index + samples->sample_count) {
        return -1;
    }

    state->null_scope_sample_count = DAB_RECEIVE_NULL_SCOPE_SAMPLE_CAPACITY;
    state->null_scope_sample_period_ns = 1000000000.0f / (float)sample_rate_hz;
    for (index = 0u; index < DAB_RECEIVE_NULL_SCOPE_SAMPLE_CAPACITY; ++index) {
        const uint64_t source = focus_start - samples->sample_index + index;
        const int16_t i = samples->iq_interleaved[2u * source];
        const int16_t q = samples->iq_interleaved[(2u * source) + 1u];
        const double power = ((double)i * i) + ((double)q * q);
        state->null_scope_iq_interleaved[2u * index] = i;
        state->null_scope_iq_interleaved[(2u * index) + 1u] = q;
        state->null_scope_power_dbfs[index] = receive_iq_power_dbfs(power);
    }

    if (receive_iq_mean_power(samples,
                              expected_start - level_window_samples,
                              level_window_samples,
                              &before_power) != 0 ||
        receive_iq_mean_power(samples,
                              expected_end + 64u,
                              level_window_samples,
                              &after_power) != 0 ||
        receive_iq_mean_power(samples,
                              expected_start + 128u,
                              null_samples - 256u,
                              &null_power) != 0) {
        state->null_scope_sample_count = 0u;
        return -1;
    }
    signal_power = before_power > after_power ? before_power : after_power;
    if (signal_power <= 0.0 || null_power <= 0.0) {
        state->null_scope_sample_count = 0u;
        return -1;
    }
    threshold_power = sqrt(signal_power * null_power);
    detected_start = expected_start;
    detected_end = expected_end;
    for (index = 0u; index <= 2u * edge_search_samples; ++index) {
        const uint64_t probe = expected_start - edge_search_samples + index;
        double window_power;
        if (receive_iq_mean_power(samples, probe, edge_window_samples, &window_power) == 0 &&
            window_power <= threshold_power) {
            detected_start = probe;
            break;
        }
    }
    for (index = 0u; index <= 2u * edge_search_samples; ++index) {
        const uint64_t probe = expected_end - edge_search_samples + index;
        double window_power;
        if (receive_iq_mean_power(samples, probe, edge_window_samples, &window_power) == 0 &&
            window_power >= threshold_power) {
            detected_end = probe;
            break;
        }
    }
    if (detected_end <= detected_start) {
        detected_start = expected_start;
        detected_end = expected_end;
    }

    state->null_scope_start_offset_samples = (int32_t)((int64_t)focus_start -
                                                        (int64_t)detected_start);
    state->null_scope_start_index = (uint32_t)(detected_start - focus_start);
    state->null_scope_end_index = (uint32_t)(detected_end - focus_start);
    state->null_scope_duration_samples = (uint32_t)(detected_end - detected_start);
    state->null_scope_threshold_dbfs = receive_iq_power_dbfs(threshold_power);
    state->null_scope_depth_db = (float)(10.0 * log10(signal_power / null_power));
    state->null_scope_confidence = (float)(1.0 - (null_power / signal_power));
    if (state->null_scope_confidence < 0.0f) {
        state->null_scope_confidence = 0.0f;
    } else if (state->null_scope_confidence > 1.0f) {
        state->null_scope_confidence = 1.0f;
    }
    state->null_scope_energy_dbfs = receive_iq_power_dbfs(null_power);

    state->null_scope_frame_bin_count = 0u;
    state->null_scope_frame_sample_count = 0u;
    if (frame_samples != 0u &&
        expected_start >= samples->sample_index &&
        expected_start + frame_samples <= samples->sample_index + samples->sample_count) {
        for (index = 0u; index < DAB_RECEIVE_NULL_SCOPE_FRAME_BIN_COUNT; ++index) {
            const uint32_t begin = (uint32_t)(((uint64_t)index * frame_samples) /
                                              DAB_RECEIVE_NULL_SCOPE_FRAME_BIN_COUNT);
            const uint32_t end = (uint32_t)(((uint64_t)(index + 1u) * frame_samples) /
                                            DAB_RECEIVE_NULL_SCOPE_FRAME_BIN_COUNT);
            double bin_power;
            if (receive_iq_mean_power(samples,
                                      expected_start + begin,
                                      end > begin ? end - begin : 1u,
                                      &bin_power) != 0) {
                break;
            }
            state->null_scope_frame_power_dbfs[index] = receive_iq_power_dbfs(bin_power);
        }
        if (index == DAB_RECEIVE_NULL_SCOPE_FRAME_BIN_COUNT) {
            state->null_scope_frame_bin_count = DAB_RECEIVE_NULL_SCOPE_FRAME_BIN_COUNT;
            state->null_scope_frame_sample_count = frame_samples;
            state->null_scope_frame_bin_samples = (float)frame_samples /
                                                  DAB_RECEIVE_NULL_SCOPE_FRAME_BIN_COUNT;
        }
    }
    return 0;
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
    double channel_power_sum = 0.0;
    uint32_t channel_power_count = 0u;
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
    state->null_scope_sample_count = 0u;
    state->null_scope_frame_bin_count = 0u;
    state->null_scope_frame_sample_count = 0u;
    memset(state->channel_magnitude_db, 0, sizeof(state->channel_magnitude_db));
    memset(state->channel_phase_deg, 0, sizeof(state->channel_phase_deg));

    for (i = 0u; i < fft_count; ++i) {
        const uint32_t source = (i + (fft_count / 2u)) % fft_count;
        const float power = (fft_i[source] * fft_i[source]) + (fft_q[source] * fft_q[source]);
        state->spectrum_dbfs[i] = receive_power_db(power, spectrum_reference_power);
        state->cir_i[i] = 0.0f;
        state->cir_q[i] = 0.0f;
    }

    for (i = 0u; i < DAB_MODE_I_ACTIVE_CARRIERS; ++i) {
        uint32_t fft_bin;
        uint32_t carrier;
        float response_i;
        float response_q;
        float response_power;
        float window;
        const float equalizer_i = ofdm->channel_i[i];
        const float equalizer_q = ofdm->channel_q[i];
        const float denominator = (equalizer_i * equalizer_i) + (equalizer_q * equalizer_q);

        if (i == DAB_MODE_I_ACTIVE_CARRIER_HALF || denominator <= 1.0e-20f) {
            continue;
        }
        fft_bin = i < DAB_MODE_I_ACTIVE_CARRIER_HALF
            ? DAB_MODE_I_FFT_SAMPLES - DAB_MODE_I_ACTIVE_CARRIER_HALF + i
            : i - DAB_MODE_I_ACTIVE_CARRIER_HALF;
        carrier = i < DAB_MODE_I_ACTIVE_CARRIER_HALF ? i : i - 1u;
        response_i = equalizer_i / denominator;
        response_q = -equalizer_q / denominator;
        response_power = (response_i * response_i) + (response_q * response_q);
        state->channel_magnitude_db[carrier] = response_power;
        state->channel_phase_deg[carrier] = atan2f(response_q, response_i) *
            (float)(180.0 / 3.14159265358979323846);
        channel_power_sum += response_power;
        channel_power_count++;
        /*
         * Taper the measured channel response at the active-band edges before
         * the IFFT.  A rectangular 1536-carrier mask produces a sinc-shaped
         * impulse with high sidelobes that can be mistaken for propagation
         * paths.  The Hann taper trades a small amount of delay resolution for
         * substantially cleaner path separation.
         */
        window = 0.5f - (0.5f * cosf(6.2831853071795864769f *
                                     (float)i /
                                     (float)(DAB_MODE_I_ACTIVE_CARRIERS - 1u)));
        state->cir_i[fft_bin] = window * response_i;
        state->cir_q[fft_bin] = window * response_q;
    }

    {
        const float mean_channel_power = channel_power_count != 0u
            ? (float)(channel_power_sum / (double)channel_power_count)
            : 0.0f;
        for (i = 0u; i < DAB_RECEIVE_CHANNEL_RESPONSE_COUNT; ++i) {
            state->channel_magnitude_db[i] = receive_power_db(
                state->channel_magnitude_db[i],
                mean_channel_power);
        }
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
    frame->channel_magnitude_db = state->channel_magnitude_db;
    frame->channel_phase_deg = state->channel_phase_deg;
    frame->channel_response_count = DAB_RECEIVE_CHANNEL_RESPONSE_COUNT;
    frame->carrier_mer_db = state->carrier_mer_db;
    frame->carrier_mer_count = DAB_RECEIVE_CARRIER_MER_COUNT;
    frame->first_carrier = -768;
    frame->negative_carrier_count = 768u;
    frame->carrier_spacing_hz = (float)sample_rate_hz /
                                (float)DAB_RECEIVE_SPECTRUM_BIN_COUNT;
    frame->null_scope_iq_interleaved = state->null_scope_iq_interleaved;
    frame->null_scope_power_dbfs = state->null_scope_power_dbfs;
    frame->null_scope_sample_count = state->null_scope_sample_count;
    frame->null_scope_start_offset_samples = state->null_scope_start_offset_samples;
    frame->null_scope_start_index = state->null_scope_start_index;
    frame->null_scope_end_index = state->null_scope_end_index;
    frame->null_scope_duration_samples = state->null_scope_duration_samples;
    frame->null_scope_sample_period_ns = state->null_scope_sample_period_ns;
    frame->null_scope_threshold_dbfs = state->null_scope_threshold_dbfs;
    frame->null_scope_depth_db = state->null_scope_depth_db;
    frame->null_scope_confidence = state->null_scope_confidence;
    frame->null_scope_energy_dbfs = state->null_scope_energy_dbfs;
    frame->null_scope_frame_power_dbfs = state->null_scope_frame_power_dbfs;
    frame->null_scope_frame_bin_count = state->null_scope_frame_bin_count;
    frame->null_scope_frame_bin_samples = state->null_scope_frame_bin_samples;
    frame->null_scope_frame_sample_count = state->null_scope_frame_sample_count;
    state->collecting = 0;
    return 0;
}
