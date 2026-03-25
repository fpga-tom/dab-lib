#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dab/config.h"
#include "dab/core/null_detect.h"
#include "dab/core/ofdm.h"
#include "dab/core/prs_acquire.h"
#include "dab/core/prs_fft.h"
#include "dab/core/prs_score.h"
#include "dab/core/sync.h"
#include "dab/core/tframe.h"
#include "dab/hal/sdr_device.h"
#include "dab/hal/sdrplay_device.h"
#include "dab/simd/simd.h"

typedef struct {
    int16_t *iq;
    uint32_t capacity_samples;
    uint32_t sample_count;
    uint64_t start_sample_index;
} history_buffer_t;

typedef struct {
    int pending;
    int predicted;
    dab_null_event_t event;
    dab_prs_candidate_t candidate;
} pending_candidate_t;

static void init_predicted_candidate(const dab_prs_acquire_t *acquire,
                                     const dab_sync_t *sync,
                                     dab_prs_candidate_t *candidate) {
    uint64_t prs_fft_start_sample;

    if (acquire == 0 || sync == 0 || candidate == 0) {
        return;
    }

    prs_fft_start_sample = dab_sync_predict_next_frame_start(sync);
    candidate->prs_fft_start_sample = prs_fft_start_sample;
    candidate->prs_symbol_start_sample = prs_fft_start_sample - acquire->config.guard_samples;
    candidate->null_start_sample = candidate->prs_symbol_start_sample - acquire->config.null_samples;
    candidate->timing_search_start_offset = sync->config.track_timing_window_samples / 2u;
    candidate->timing_search_length = sync->config.track_timing_window_samples;
}

static unsigned int env_u32(const char *name, unsigned int fallback) {
    const char *value = getenv(name);
    char *end = 0;
    unsigned long parsed;

    if (value == 0 || value[0] == '\0') {
        return fallback;
    }
    parsed = strtoul(value, &end, 10);
    if (end == value || *end != '\0') {
        return fallback;
    }
    return (unsigned int)parsed;
}

static int env_i32(const char *name, int fallback) {
    const char *value = getenv(name);
    char *end = 0;
    long parsed;

    if (value == 0 || value[0] == '\0') {
        return fallback;
    }
    parsed = strtol(value, &end, 10);
    if (end == value || *end != '\0') {
        return fallback;
    }
    return (int)parsed;
}

static unsigned long long env_u64(const char *name, unsigned long long fallback) {
    const char *value = getenv(name);
    char *end = 0;
    unsigned long long parsed;

    if (value == 0 || value[0] == '\0') {
        return fallback;
    }
    parsed = strtoull(value, &end, 10);
    if (end == value || *end != '\0') {
        return fallback;
    }
    return parsed;
}

static float env_f32(const char *name, float fallback) {
    const char *value = getenv(name);
    char *end = 0;
    float parsed;

    if (value == 0 || value[0] == '\0') {
        return fallback;
    }
    parsed = strtof(value, &end);
    if (end == value || *end != '\0') {
        return fallback;
    }
    return parsed;
}

static dab_sdrplay_gain_mode_t env_gain_mode(const char *name, dab_sdrplay_gain_mode_t fallback) {
    const char *value = getenv(name);

    if (value == 0 || value[0] == '\0') {
        return fallback;
    }
    if (strcmp(value, "auto") == 0) {
        return DAB_SDRPLAY_GAIN_MODE_AUTO;
    }
    if (strcmp(value, "manual") == 0) {
        return DAB_SDRPLAY_GAIN_MODE_MANUAL;
    }
    return fallback;
}

static int history_init(history_buffer_t *history, uint32_t capacity_samples) {
    if (history == 0 || capacity_samples == 0u) {
        return -1;
    }
    history->iq = (int16_t *)calloc((size_t)capacity_samples * 2u, sizeof(*history->iq));
    if (history->iq == 0) {
        return -1;
    }
    history->capacity_samples = capacity_samples;
    history->sample_count = 0u;
    history->start_sample_index = 0u;
    return 0;
}

static void history_destroy(history_buffer_t *history) {
    if (history == 0) {
        return;
    }
    free(history->iq);
    history->iq = 0;
    history->capacity_samples = 0u;
    history->sample_count = 0u;
    history->start_sample_index = 0u;
}

static void history_reset(history_buffer_t *history, const dab_sdr_block_view_t *block) {
    uint32_t keep;

    if (history == 0 || block == 0 || history->iq == 0 || block->iq_interleaved == 0) {
        return;
    }

    keep = block->sample_count > history->capacity_samples ? history->capacity_samples : block->sample_count;
    memcpy(history->iq,
           block->iq_interleaved + (2u * (block->sample_count - keep)),
           sizeof(*history->iq) * 2u * keep);
    history->sample_count = keep;
    history->start_sample_index = block->sample_index + (block->sample_count - keep);
}

static void history_append(history_buffer_t *history, const dab_sdr_block_view_t *block) {
    uint32_t drop;
    uint32_t keep;
    uint64_t expected_next;

    if (history == 0 || block == 0 || history->iq == 0 || block->iq_interleaved == 0) {
        return;
    }
    if (history->sample_count == 0u) {
        history_reset(history, block);
        return;
    }

    expected_next = history->start_sample_index + history->sample_count;
    if (block->sample_index != expected_next) {
        history_reset(history, block);
        return;
    }

    keep = block->sample_count;
    if (keep > history->capacity_samples) {
        history_reset(history, block);
        return;
    }

    if ((history->sample_count + keep) > history->capacity_samples) {
        drop = (history->sample_count + keep) - history->capacity_samples;
        memmove(history->iq,
                history->iq + (2u * drop),
                sizeof(*history->iq) * 2u * (history->sample_count - drop));
        history->sample_count -= drop;
        history->start_sample_index += drop;
    }

    memcpy(history->iq + (2u * history->sample_count),
           block->iq_interleaved,
           sizeof(*history->iq) * 2u * keep);
    history->sample_count += keep;
}

static dab_sdr_block_view_t history_view(const history_buffer_t *history) {
    dab_sdr_block_view_t view = {0};

    if (history == 0) {
        return view;
    }
    view.iq_interleaved = history->iq;
    view.sample_count = history->sample_count;
    view.sample_index = history->start_sample_index;
    view.timestamp_ns = 0u;
    return view;
}

static int history_contains_sample_range(const history_buffer_t *history,
                                         uint64_t start_sample,
                                         uint64_t end_sample) {
    uint64_t history_end;

    if (history == 0 || history->sample_count == 0u) {
        return 0;
    }
    history_end = history->start_sample_index + history->sample_count;
    return start_sample >= history->start_sample_index && end_sample <= history_end;
}

int main(void) {
    dab_sdr_open_params_t params = {"sdrplay", NULL, NULL};
    dab_sdr_device_t *device = 0;
    dab_sdr_stream_t *stream = 0;
    dab_stream_config_t stream_config;
    dab_sdrplay_config_t device_config;
    dab_null_detect_t detector;
    dab_null_detect_config_t detector_config;
    dab_prs_acquire_t acquire;
    dab_prs_acquire_config_t acquire_config;
    dab_prs_score_t cp_score;
    dab_prs_score_config_t score_config;
    dab_prs_fft_t fft;
    dab_prs_fft_config_t fft_config;
    dab_ofdm_t ofdm;
    dab_ofdm_config_t ofdm_config;
    dab_sync_t sync;
    dab_sync_config_t sync_config;
    dab_tframe_t tframe;
    dab_tframe_buffers_t tframe_buffers;
    history_buffer_t history = {0};
    pending_candidate_t pending = {0};
    dab_sdr_block_view_t src_block;
    dab_null_event_t event;
    const dab_simd_ops_t *simd_ops = dab_simd_get_ops();
    float *fft_i = 0;
    float *fft_q = 0;
    float *active_i = 0;
    float *active_q = 0;
    float *prev_active_i = 0;
    float *prev_active_q = 0;
    float *diff_i = 0;
    float *diff_q = 0;
    float *deintl_i = 0;
    float *deintl_q = 0;
    int8_t *soft_a = 0;
    int8_t *soft_b = 0;
    int8_t *fic_softbits = 0;
    int8_t *msc_softbits = 0;
    int8_t *frame_softbits = 0;
    unsigned int blocks_to_read = env_u32("DAB_SYNC_PROBE_BLOCKS", 400u);
    unsigned int block_num;
    unsigned int dropped_blocks = 0u;
    uint64_t last_predicted_fft_start = 0u;
    float ref_threshold = env_f32("DAB_SYNC_PROBE_REF_SCORE_THRESHOLD", 0.10f);
    int rc = 1;

    dab_sdrplay_default_config(&device_config);
    device_config.gain_mode = env_gain_mode("DAB_SYNC_PROBE_GAIN_MODE", DAB_SDRPLAY_GAIN_MODE_MANUAL);
    device_config.if_mode = DAB_SDRPLAY_IF_MODE_ZERO_IF;
    device_config.antenna = DAB_SDRPLAY_ANTENNA_A;
    device_config.lna_state = (unsigned char)env_u32("DAB_SYNC_PROBE_LNA_STATE", 0u);
    device_config.if_gain_db = env_i32("DAB_SYNC_PROBE_IF_GAIN_DB", 0);
    device_config.bias_tee_enabled = env_i32("DAB_SYNC_PROBE_BIAS_TEE", 0);
    device_config.ppm_correction = env_i32("DAB_SYNC_PROBE_PPM", 0);

    stream_config.sample_rate = env_u32("DAB_SYNC_PROBE_SAMPLE_RATE", 2048000u);
    stream_config.center_freq_hz = env_u64("DAB_SYNC_PROBE_CENTER_FREQ_HZ", 220352000ull);
    stream_config.bandwidth_hz = env_u32("DAB_SYNC_PROBE_BANDWIDTH_HZ", 1536000u);
    stream_config.gain_db = env_i32("DAB_SYNC_PROBE_GAIN_DB", 40);

    dab_null_detect_default_config(&detector_config);
    detector_config.fast_window_samples = env_u32("DAB_SYNC_PROBE_FAST_WINDOW_SAMPLES", 128u);
    detector_config.slow_window_samples = env_u32("DAB_SYNC_PROBE_SLOW_WINDOW_SAMPLES", 8192u);
    detector_config.sample_decimation = env_u32("DAB_SYNC_PROBE_SAMPLE_DECIMATION", 4u);
    detector_config.enter_ratio_threshold = env_f32("DAB_SYNC_PROBE_ENTER_RATIO_THRESHOLD", 0.35f);
    detector_config.exit_ratio_threshold = env_f32("DAB_SYNC_PROBE_EXIT_RATIO_THRESHOLD", 0.55f);
    detector_config.min_null_samples = env_u32("DAB_SYNC_PROBE_MIN_NULL_SAMPLES", 1800u);
    detector_config.max_null_samples = env_u32("DAB_SYNC_PROBE_MAX_NULL_SAMPLES", 4000u);
    detector_config.startup_ignore_samples = env_u32("DAB_SYNC_PROBE_STARTUP_IGNORE_SAMPLES", 0u);
    detector_config.max_merge_gap_samples = env_u32("DAB_SYNC_PROBE_MAX_MERGE_GAP_SAMPLES", 0u);
    if (dab_null_detect_init(&detector, &detector_config) != 0) {
        fprintf(stderr, "null detector init failed\n");
        return 2;
    }

    dab_prs_acquire_default_config(&acquire_config);
    acquire_config.acquire_window_samples = env_u32("DAB_SYNC_PROBE_ACQUIRE_WINDOW_SAMPLES",
                                                    acquire_config.acquire_window_samples);
    if (dab_prs_acquire_init(&acquire, &acquire_config) != 0) {
        fprintf(stderr, "prs acquire init failed\n");
        return 2;
    }

    dab_prs_score_default_config(&score_config);
    score_config.sample_rate = stream_config.sample_rate;
    score_config.guard_samples = acquire_config.guard_samples;
    score_config.fft_samples = acquire_config.fft_samples;
    if (dab_prs_score_init(&cp_score, &score_config) != 0) {
        fprintf(stderr, "prs score init failed\n");
        return 2;
    }

    dab_prs_fft_default_config(&fft_config);
    fft_config.fft_samples = acquire_config.fft_samples;
    fft_config.sample_rate = stream_config.sample_rate;
    if (dab_prs_fft_init(&fft, &fft_config) != 0) {
        fprintf(stderr, "prs fft init failed\n");
        return 2;
    }

    dab_sync_default_config(&sync_config);
    sync_config.acquire_timing_window_samples = acquire_config.acquire_window_samples;
    sync_config.track_timing_window_samples = env_u32("DAB_SYNC_PROBE_TRACK_WINDOW_SAMPLES",
                                                      sync_config.track_timing_window_samples);
    sync_config.acquire_prs_score_threshold = env_f32("DAB_SYNC_PROBE_ACQUIRE_PRS_SCORE_THRESHOLD",
                                                      ref_threshold);
    sync_config.track_prs_score_threshold = env_f32("DAB_SYNC_PROBE_TRACK_PRS_SCORE_THRESHOLD",
                                                    ref_threshold * 0.8f);
    if (dab_sync_init(&sync, &sync_config) != 0) {
        fprintf(stderr, "sync init failed\n");
        return 2;
    }

    dab_ofdm_default_config(&ofdm_config);
    if (dab_ofdm_init(&ofdm, &ofdm_config) != 0) {
        fprintf(stderr, "ofdm init failed\n");
        return 2;
    }

    fft_i = (float *)calloc(acquire_config.fft_samples, sizeof(*fft_i));
    fft_q = (float *)calloc(acquire_config.fft_samples, sizeof(*fft_q));
    active_i = (float *)calloc(DAB_MODE_I_ACTIVE_CARRIERS, sizeof(*active_i));
    active_q = (float *)calloc(DAB_MODE_I_ACTIVE_CARRIERS, sizeof(*active_q));
    prev_active_i = (float *)calloc(DAB_MODE_I_ACTIVE_CARRIERS, sizeof(*prev_active_i));
    prev_active_q = (float *)calloc(DAB_MODE_I_ACTIVE_CARRIERS, sizeof(*prev_active_q));
    diff_i = (float *)calloc(DAB_MODE_I_DATA_CARRIERS, sizeof(*diff_i));
    diff_q = (float *)calloc(DAB_MODE_I_DATA_CARRIERS, sizeof(*diff_q));
    deintl_i = (float *)calloc(DAB_MODE_I_DATA_CARRIERS, sizeof(*deintl_i));
    deintl_q = (float *)calloc(DAB_MODE_I_DATA_CARRIERS, sizeof(*deintl_q));
    soft_a = (int8_t *)calloc(DAB_MODE_I_DATA_CARRIERS, sizeof(*soft_a));
    soft_b = (int8_t *)calloc(DAB_MODE_I_DATA_CARRIERS, sizeof(*soft_b));
    fic_softbits = (int8_t *)calloc(DAB_TFRAME_FIC_SOFTBITS, sizeof(*fic_softbits));
    msc_softbits = (int8_t *)calloc(DAB_TFRAME_MSC_SOFTBITS, sizeof(*msc_softbits));
    frame_softbits = (int8_t *)calloc(DAB_TFRAME_TOTAL_SOFTBITS, sizeof(*frame_softbits));
    if (fft_i == 0 || fft_q == 0 || active_i == 0 || active_q == 0 ||
        prev_active_i == 0 || prev_active_q == 0 || diff_i == 0 || diff_q == 0 ||
        deintl_i == 0 || deintl_q == 0 || soft_a == 0 || soft_b == 0 ||
        fic_softbits == 0 || msc_softbits == 0 || frame_softbits == 0) {
        fprintf(stderr, "alloc failed\n");
        goto cleanup;
    }
    ofdm.prev_active_i = prev_active_i;
    ofdm.prev_active_q = prev_active_q;
    tframe_buffers.fic_softbits = fic_softbits;
    tframe_buffers.msc_softbits = msc_softbits;
    tframe_buffers.frame_softbits = frame_softbits;
    if (dab_tframe_init(&tframe, &tframe_buffers) != 0) {
        fprintf(stderr, "tframe init failed\n");
        goto cleanup;
    }

    if (history_init(&history, env_u32("DAB_SYNC_PROBE_HISTORY_SAMPLES", 262144u)) != 0) {
        fprintf(stderr, "history init failed\n");
        goto cleanup;
    }

    fprintf(stderr, "open device\n");
    if (dab_sdr_device_open(&params, &device) != DAB_SDR_STATUS_OK || device == 0) {
        fprintf(stderr, "open failed\n");
        goto cleanup;
    }
    fprintf(stderr, "configure device\n");
    if (dab_sdrplay_configure_device(device, &device_config) != DAB_SDR_STATUS_OK) {
        fprintf(stderr, "device config failed\n");
        goto cleanup;
    }
    fprintf(stderr, "configure stream\n");
    if (device->vtbl->configure(device, &stream_config) != DAB_SDR_STATUS_OK) {
        fprintf(stderr, "stream config failed\n");
        goto cleanup;
    }
    fprintf(stderr, "open stream\n");
    if (device->vtbl->open_stream(device, &stream) != DAB_SDR_STATUS_OK || stream == 0) {
        fprintf(stderr, "open stream failed\n");
        goto cleanup;
    }
    fprintf(stderr, "start stream\n");
    if (stream->vtbl->start(stream) != DAB_SDR_STATUS_OK) {
        fprintf(stderr, "start failed\n");
        goto cleanup;
    }

    printf("sync-probe start sample_rate=%u center_freq_hz=%llu blocks=%u simd=%s fft=%s ref_threshold=%.3f\n",
           stream_config.sample_rate,
           (unsigned long long)stream_config.center_freq_hz,
           blocks_to_read,
           simd_ops != 0 ? simd_ops->name : "unknown",
           fft.backend_name != 0 ? fft.backend_name : "unknown",
           ref_threshold);

    for (block_num = 0u; block_num < blocks_to_read; ++block_num) {
        dab_sdr_block_view_t hist_view;

        if (stream->vtbl->read(stream, &src_block) != DAB_SDR_STATUS_OK) {
            fprintf(stderr, "read failed at block %u\n", block_num);
            goto cleanup;
        }
        history_append(&history, &src_block);

        if (dab_null_detect_process_i16(&detector, &src_block, &event) < 0) {
            fprintf(stderr, "null detector failed at block %u\n", block_num);
            goto cleanup;
        }

        if (event.length != 0u && !sync.state.locked) {
            if (dab_prs_candidate_from_null(&acquire, &event, &pending.candidate) == 0) {
                pending.pending = 1;
                pending.predicted = 0;
                pending.event = event;
                printf("null sample_index=%llu length=%u candidate_fft=%llu search_start=%llu search_end=%llu\n",
                       (unsigned long long)event.sample_index,
                       event.length,
                       (unsigned long long)pending.candidate.prs_fft_start_sample,
                       (unsigned long long)dab_prs_candidate_search_start_sample(&pending.candidate),
                       (unsigned long long)dab_prs_candidate_search_end_sample(&pending.candidate));
            }
        }

        if (!pending.pending && sync.state.locked) {
            dab_prs_candidate_t predicted_candidate = {0};

            init_predicted_candidate(&acquire, &sync, &predicted_candidate);
            if (predicted_candidate.prs_fft_start_sample != last_predicted_fft_start) {
                pending.pending = 1;
                pending.predicted = 1;
                pending.candidate = predicted_candidate;
                pending.event = (dab_null_event_t){0};
                last_predicted_fft_start = predicted_candidate.prs_fft_start_sample;
                printf("predict candidate_fft=%llu search_start=%llu search_end=%llu\n",
                       (unsigned long long)pending.candidate.prs_fft_start_sample,
                       (unsigned long long)dab_prs_candidate_search_start_sample(&pending.candidate),
                       (unsigned long long)dab_prs_candidate_search_end_sample(&pending.candidate));
            }
        }

        if (!pending.pending) {
            continue;
        }

        {
            uint64_t required_end = dab_prs_candidate_search_end_sample(&pending.candidate) + acquire_config.fft_samples;
            if (sync.state.locked) {
                required_end += ((uint64_t)DAB_TFRAME_TOTAL_SYMBOLS * acquire_config.symbol_samples);
            }
            if (!history_contains_sample_range(&history,
                                               dab_prs_candidate_search_start_sample(&pending.candidate),
                                               required_end)) {
                continue;
            }
        }

        hist_view = history_view(&history);
        {
            dab_prs_result_t coarse = {0};
            dab_prs_reference_score_t ref_score = {0};
            int coarse_rc = dab_prs_score_search_i16(&cp_score, &hist_view, &pending.candidate, &coarse);

            if (coarse_rc == 1) {
                float cfo_for_fft = (float)coarse.coarse_freq_error_hz;
                if (sync.state.locked && sync.state.bad_frames == 0u) {
                    cfo_for_fft = (float)sync.state.coarse_freq_error_hz;
                }
                if (dab_prs_score_symbol_i16_mode_i_diff_cfo(&fft,
                                                             &hist_view,
                                                             coarse.prs_fft_start_sample,
                                                             cfo_for_fft,
                                                             fft_i,
                                                             fft_q,
                                                             &ref_score) == 0) {
                    int sync_rc;
                    if (ref_score.carrier_offset_bins != 0) {
                        (void)dab_prs_fft_apply_bin_shift(fft_i,
                                                          fft_q,
                                                          acquire_config.fft_samples,
                                                          -ref_score.carrier_offset_bins);
                    }
                    if (dab_ofdm_extract_active_carriers_mode_i(fft_i, fft_q, active_i, active_q) != 0) {
                        pending.pending = 0;
                        pending.predicted = 0;
                        continue;
                    }
                    printf("prs fft_start=%llu cp_score=%.6f ref_score=%.6f ifo=%d cfo=%d mode=%d locked=%d\n",
                           (unsigned long long)coarse.prs_fft_start_sample,
                           coarse.prs_score,
                           ref_score.score,
                           ref_score.carrier_offset_bins,
                           coarse.coarse_freq_error_hz,
                           sync.state.mode,
                           sync.state.locked);

                    if (sync.state.locked) {
                        sync_rc = dab_sync_on_prs_track(&sync,
                                                        coarse.prs_fft_start_sample,
                                                        ref_score.score,
                                                        0.0f,
                                                        ref_score.carrier_offset_bins);
                    } else {
                        dab_sync_on_null_candidate(&sync, &pending.event);
                        sync_rc = dab_sync_on_prs_acquire(&sync,
                                                          coarse.prs_fft_start_sample,
                                                          ref_score.score,
                                                          coarse.coarse_freq_error_hz,
                                                          ref_score.carrier_offset_bins);
                    }

                    if (!sync.state.locked) {
                        last_predicted_fft_start = 0u;
                        dab_ofdm_reset(&ofdm);
                        dab_tframe_reset(&tframe);
                        if (pending.predicted) {
                            printf("track-loss source=predict bad=%u mode=%d locked=%d\n",
                                   sync.state.bad_frames,
                                   sync.state.mode,
                                   sync.state.locked);
                        }
                    }

                    if (ref_score.score >= ref_threshold) {
                        printf("sync-update rc=%d source=%s frame_start=%llu prs_score=%.6f bad=%u good=%u mode=%d locked=%d timing_error=%d cfo=%d ifo=%d\n",
                               sync_rc,
                               pending.predicted ? "predict" : "null",
                               (unsigned long long)sync.state.frame_start_sample,
                               sync.state.prs_score,
                               sync.state.bad_frames,
                               sync.state.good_frames,
                               sync.state.mode,
                               sync.state.locked,
                               sync.state.timing_error_samples,
                               sync.state.coarse_freq_error_hz,
                               sync.state.carrier_offset_bins);
                    }

                    {
                        dab_ofdm_buffers_t ofdm_buffers;
                        dab_ofdm_result_t ofdm_result;
                        uint64_t data_fft_start;
                        dab_tframe_result_t tframe_result;
                        unsigned int symbol_idx;

                        ofdm_buffers.active_i = active_i;
                        ofdm_buffers.active_q = active_q;
                        ofdm_buffers.diff_i = diff_i;
                        ofdm_buffers.diff_q = diff_q;
                        ofdm_buffers.deintl_i = deintl_i;
                        ofdm_buffers.deintl_q = deintl_q;
                        ofdm_buffers.soft_a = soft_a;
                        ofdm_buffers.soft_b = soft_b;

                        dab_ofdm_reset(&ofdm);
                        dab_tframe_reset(&tframe);
                        (void)dab_ofdm_process_mode_i(&ofdm,
                                                      fft_i,
                                                      fft_q,
                                                      coarse.prs_fft_start_sample,
                                                      DAB_OFDM_SYMBOL_PRS,
                                                      &ofdm_buffers,
                                                      &ofdm_result);

                        for (symbol_idx = 0u; symbol_idx < DAB_TFRAME_TOTAL_SYMBOLS; ++symbol_idx) {
                            data_fft_start = coarse.prs_fft_start_sample +
                                             ((uint64_t)(symbol_idx + 1u) * acquire_config.symbol_samples);
                            if (!history_contains_sample_range(&history,
                                                               data_fft_start,
                                                               data_fft_start + acquire_config.fft_samples)) {
                                break;
                            }
                            if (dab_prs_fft_execute_i16_cfo(&fft,
                                                            &hist_view,
                                                            data_fft_start,
                                                            cfo_for_fft,
                                                            fft_i,
                                                            fft_q) != 0) {
                                break;
                            }
                            if (ref_score.carrier_offset_bins != 0) {
                                (void)dab_prs_fft_apply_bin_shift(fft_i,
                                                                  fft_q,
                                                                  acquire_config.fft_samples,
                                                                  -ref_score.carrier_offset_bins);
                            }
                            if (dab_ofdm_process_mode_i(&ofdm,
                                                        fft_i,
                                                        fft_q,
                                                        data_fft_start,
                                                        DAB_OFDM_SYMBOL_DATA,
                                                        &ofdm_buffers,
                                                        &ofdm_result) != 0) {
                                break;
                            }
                            if (ofdm_result.have_diff) {
                                const float diff_power = (diff_i[0] * diff_i[0]) + (diff_q[0] * diff_q[0]);
                                printf("ofdm data_fft_start=%llu diff0_re=%.6f diff0_im=%.6f diff0_power=%.6f",
                                       (unsigned long long)data_fft_start,
                                       diff_i[0],
                                       diff_q[0],
                                       diff_power);
                                if (ofdm_result.have_softbits) {
                                    printf(" soft0=(%d,%d) soft1=(%d,%d)",
                                           (int)soft_a[0],
                                           (int)soft_b[0],
                                           (int)soft_a[1],
                                           (int)soft_b[1]);
                                    if (dab_tframe_push_symbol(&tframe,
                                                               data_fft_start,
                                                               soft_a,
                                                               soft_b,
                                                               &tframe_result) == 0) {
                                        printf(" tframe_symbol=%u frame_complete=%d",
                                               tframe_result.symbol_index,
                                               tframe_result.frame_complete);
                                    }
                                }
                                printf("\n");
                            }
                        }
                    }
                }
            }
        }

        pending.pending = 0;
        pending.predicted = 0;
    }

    dropped_blocks = dab_sdrplay_stream_dropped_blocks(stream);
    printf("summary blocks=%u dropped_blocks=%u mode=%d locked=%d frame_start=%llu prs_score=%.6f cfo=%d ifo=%d\n",
           blocks_to_read,
           dropped_blocks,
           sync.state.mode,
           sync.state.locked,
           (unsigned long long)sync.state.frame_start_sample,
           sync.state.prs_score,
           sync.state.coarse_freq_error_hz,
           sync.state.carrier_offset_bins);
    rc = 0;

cleanup:
    if (stream != 0) {
        (void)stream->vtbl->stop(stream);
        dab_sdr_stream_close(stream);
    }
    if (device != 0) {
        dab_sdr_device_close(device);
    }
    history_destroy(&history);
    free(frame_softbits);
    free(msc_softbits);
    free(fic_softbits);
    free(soft_b);
    free(soft_a);
    free(deintl_q);
    free(deintl_i);
    free(diff_q);
    free(diff_i);
    free(prev_active_q);
    free(prev_active_i);
    free(active_q);
    free(active_i);
    free(fft_q);
    free(fft_i);
    return rc;
}
