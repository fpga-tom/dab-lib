#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dab/config.h"
#include "dab/core/frontend.h"
#include "dab/core/null_detect.h"
#include "dab/hal/sdr_device.h"
#include "dab/hal/sdrplay_device.h"

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

int main(void) {
    dab_sdr_open_params_t params = {"sdrplay", NULL, NULL};
    dab_sdr_device_t *device = 0;
    dab_sdr_stream_t *stream = 0;
    dab_stream_config_t stream_config;
    dab_sdrplay_config_t device_config;
    dab_frontend_t frontend;
    dab_frontend_config_t frontend_config;
    dab_null_detect_t detector;
    dab_null_detect_config_t detector_config;
    dab_sdr_block_view_t src_block;
    dab_frontend_block_t frontend_block;
    dab_null_event_t event;
    float i_data[DAB_IQ_BLOCK_SAMPLES];
    float q_data[DAB_IQ_BLOCK_SAMPLES];
    unsigned int blocks_to_read = env_u32("DAB_NULL_PROBE_BLOCKS", 200u);
    unsigned int log_every_blocks = env_u32("DAB_NULL_PROBE_LOG_EVERY_BLOCKS", 0u);
    unsigned int expected_null_interval = env_u32("DAB_NULL_PROBE_EXPECTED_NULL_INTERVAL", 0u);
    unsigned int block_num;
    unsigned int was_in_run = 0u;
    uint64_t prev_null_sample_index = 0u;
    unsigned int has_prev_null = 0u;
    unsigned int dropped_blocks = 0u;
    int rc = 1;

    dab_sdrplay_default_config(&device_config);
    device_config.gain_mode = env_gain_mode("DAB_NULL_PROBE_GAIN_MODE", DAB_SDRPLAY_GAIN_MODE_MANUAL);
    device_config.if_mode = DAB_SDRPLAY_IF_MODE_ZERO_IF;
    device_config.antenna = DAB_SDRPLAY_ANTENNA_A;
    device_config.lna_state = (unsigned char)env_u32("DAB_NULL_PROBE_LNA_STATE", 0u);
    device_config.if_gain_db = env_i32("DAB_NULL_PROBE_IF_GAIN_DB", 0);
    device_config.bias_tee_enabled = env_i32("DAB_NULL_PROBE_BIAS_TEE", 0);
    device_config.ppm_correction = env_i32("DAB_NULL_PROBE_PPM", 0);

    stream_config.sample_rate = env_u32("DAB_NULL_PROBE_SAMPLE_RATE", 2048000u);
    stream_config.center_freq_hz = env_u64("DAB_NULL_PROBE_CENTER_FREQ_HZ", 220352000ull);
    stream_config.bandwidth_hz = env_u32("DAB_NULL_PROBE_BANDWIDTH_HZ", 1536000u);
    stream_config.gain_db = env_i32("DAB_NULL_PROBE_GAIN_DB", 40);

    dab_frontend_default_config(&frontend_config);
    frontend_config.dc_alpha = env_f32("DAB_NULL_PROBE_DC_ALPHA", frontend_config.dc_alpha);
    if (dab_frontend_init(&frontend, &frontend_config) != 0) {
        fprintf(stderr, "frontend init failed\n");
        return 2;
    }

    dab_null_detect_default_config(&detector_config);
    detector_config.fast_alpha = env_f32("DAB_NULL_PROBE_FAST_ALPHA", detector_config.fast_alpha);
    detector_config.slow_alpha = env_f32("DAB_NULL_PROBE_SLOW_ALPHA", detector_config.slow_alpha);
    detector_config.fast_window_samples = env_u32("DAB_NULL_PROBE_FAST_WINDOW_SAMPLES",
                                                  detector_config.fast_window_samples);
    detector_config.slow_window_samples = env_u32("DAB_NULL_PROBE_SLOW_WINDOW_SAMPLES",
                                                  detector_config.slow_window_samples);
    detector_config.enter_ratio_threshold = env_f32("DAB_NULL_PROBE_ENTER_RATIO_THRESHOLD", 0.3f);
    detector_config.exit_ratio_threshold = env_f32("DAB_NULL_PROBE_EXIT_RATIO_THRESHOLD", 0.5f);
    detector_config.min_null_samples = env_u32("DAB_NULL_PROBE_MIN_NULL_SAMPLES", 1u);
    detector_config.max_null_samples = env_u32("DAB_NULL_PROBE_MAX_NULL_SAMPLES", 0u);
    detector_config.startup_ignore_samples = env_u32("DAB_NULL_PROBE_STARTUP_IGNORE_SAMPLES", 0u);
    detector_config.expected_interval_samples = expected_null_interval;
    detector_config.interval_tolerance_samples = env_u32("DAB_NULL_PROBE_INTERVAL_TOLERANCE_SAMPLES", 0u);
    detector_config.max_merge_gap_samples = env_u32("DAB_NULL_PROBE_MAX_MERGE_GAP_SAMPLES", 0u);
    if (dab_null_detect_init(&detector, &detector_config) != 0) {
        fprintf(stderr, "null detector init failed\n");
        return 2;
    }

    frontend_block.i_data = i_data;
    frontend_block.q_data = q_data;

    fprintf(stderr, "open device\n");
    if (dab_sdr_device_open(&params, &device) != DAB_SDR_STATUS_OK || device == 0) {
        fprintf(stderr, "open failed\n");
        return 2;
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

    printf("null-probe start sample_rate=%u center_freq_hz=%llu bandwidth_hz=%u blocks=%u gain_mode=%s fast_window=%u slow_window=%u fast_alpha=%.4f slow_alpha=%.4f enter_ratio=%.4f exit_ratio=%.4f expected_null_interval=%u\n",
           stream_config.sample_rate,
           (unsigned long long)stream_config.center_freq_hz,
           stream_config.bandwidth_hz,
           blocks_to_read,
           device_config.gain_mode == DAB_SDRPLAY_GAIN_MODE_AUTO ? "auto" : "manual",
           detector.config.fast_window_samples,
           detector.config.slow_window_samples,
           detector_config.fast_alpha,
           detector_config.slow_alpha,
           detector_config.enter_ratio_threshold,
           detector_config.exit_ratio_threshold,
           expected_null_interval);

    for (block_num = 0u; block_num < blocks_to_read; ++block_num) {
        if (stream->vtbl->read(stream, &src_block) != DAB_SDR_STATUS_OK) {
            fprintf(stderr, "read failed at block %u\n", block_num);
            goto cleanup;
        }
        if (src_block.sample_count > DAB_IQ_BLOCK_SAMPLES) {
            fprintf(stderr, "block too large: %u\n", src_block.sample_count);
            goto cleanup;
        }
        if (dab_frontend_process(&frontend, &src_block, &frontend_block) != 0) {
            fprintf(stderr, "frontend failed at block %u\n", block_num);
            goto cleanup;
        }

        if (dab_null_detect_process(&detector, &frontend_block, &event) < 0) {
            fprintf(stderr, "null detector failed at block %u\n", block_num);
            goto cleanup;
        }

        if (detector.run_length != 0u && was_in_run == 0u) {
            printf("drop-start block=%u sample_index=%llu fast_power=%.3f slow_power=%.3f ratio=%.6f\n",
                   block_num,
                   (unsigned long long)(detector.run_start_index),
                   detector.fast_power,
                   detector.slow_power,
                   detector.slow_power > 1e-12f ? (detector.fast_power / detector.slow_power) : 0.0f);
        }

        if (log_every_blocks != 0u && (block_num % log_every_blocks) == 0u) {
            printf("status block=%u sample_index=%llu samples=%u fast_power=%.3f slow_power=%.3f ratio=%.6f run_length=%u\n",
                   block_num,
                   (unsigned long long)frontend_block.sample_index,
                   frontend_block.sample_count,
                   detector.fast_power,
                   detector.slow_power,
                   detector.slow_power > 1e-12f ? (detector.fast_power / detector.slow_power) : 0.0f,
                   detector.run_length);
        }

        if (event.length != 0u) {
            if (has_prev_null && expected_null_interval != 0u) {
                const uint64_t delta = event.sample_index - prev_null_sample_index;
                const long long error = (long long)delta - (long long)expected_null_interval;
                printf("drop sample_index=%llu length=%u avg_power=%.6f min_ratio=%.6f delta=%llu expected=%u error=%lld\n",
                       (unsigned long long)event.sample_index,
                       event.length,
                       event.avg_power,
                       event.min_ratio,
                       (unsigned long long)delta,
                       expected_null_interval,
                       error);
            } else if (expected_null_interval != 0u) {
                printf("drop sample_index=%llu length=%u avg_power=%.6f min_ratio=%.6f delta=na expected=%u error=na\n",
                       (unsigned long long)event.sample_index,
                       event.length,
                       event.avg_power,
                       event.min_ratio,
                       expected_null_interval);
            } else {
                printf("drop sample_index=%llu length=%u avg_power=%.6f min_ratio=%.6f\n",
                       (unsigned long long)event.sample_index,
                       event.length,
                       event.avg_power,
                       event.min_ratio);
            }
            prev_null_sample_index = event.sample_index;
            has_prev_null = 1u;
        } else if (was_in_run != 0u && detector.run_length == 0u) {
            printf("drop-end block=%u sample_index=%llu\n",
                   block_num,
                   (unsigned long long)(frontend_block.sample_index + frontend_block.sample_count));
        }

        was_in_run = detector.run_length != 0u;
    }

    dropped_blocks = dab_sdrplay_stream_dropped_blocks(stream);
    printf("summary blocks=%u dropped_blocks=%u\n", blocks_to_read, dropped_blocks);
    rc = 0;

cleanup:
    if (stream != 0) {
        (void)stream->vtbl->stop(stream);
        dab_sdr_stream_close(stream);
    }
    if (device != 0) {
        dab_sdr_device_close(device);
    }
    return rc;
}
