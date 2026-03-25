#include "dab/core/null_detect.h"

#include <string.h>

#include "dab/simd/simd.h"

static uint32_t dab_null_detect_alpha_to_window(float alpha) {
    uint32_t window;

    if (alpha <= 0.0f || alpha >= 1.0f) {
        return 0u;
    }

    window = (uint32_t)((2.0f / alpha) - 1.0f + 0.5f);
    return window < 1u ? 1u : window;
}

static int dab_null_detect_is_power_of_two(uint32_t value) {
    return value != 0u && (value & (value - 1u)) == 0u;
}

void dab_null_detect_default_config(dab_null_detect_config_t *config) {
    if (config == 0) {
        return;
    }
    config->fast_alpha = 0.05f;
    config->slow_alpha = 0.001f;
    config->fast_window_samples = 0u;
    config->slow_window_samples = 0u;
    config->sample_decimation = 1u;
    config->enter_ratio_threshold = 0.25f;
    config->exit_ratio_threshold = 0.45f;
    config->min_null_samples = 128u;
    config->max_null_samples = 0u;
    config->startup_ignore_samples = 0u;
    config->expected_interval_samples = 0u;
    config->interval_tolerance_samples = 0u;
    config->max_merge_gap_samples = 0u;
}

int dab_null_detect_init(dab_null_detect_t *detector, const dab_null_detect_config_t *config) {
    dab_null_detect_config_t defaults;

    if (detector == 0) {
        return -1;
    }

    dab_null_detect_default_config(&defaults);
    detector->config = config != 0 ? *config : defaults;

    if (detector->config.fast_window_samples == 0u) {
        detector->config.fast_window_samples = dab_null_detect_alpha_to_window(detector->config.fast_alpha);
    }
    if (detector->config.slow_window_samples == 0u) {
        detector->config.slow_window_samples = dab_null_detect_alpha_to_window(detector->config.slow_alpha);
    }
    if (detector->config.sample_decimation == 0u) {
        return -1;
    }

    if (detector->config.fast_window_samples == 0u ||
        detector->config.fast_window_samples > DAB_NULL_DETECT_MAX_WINDOW_SAMPLES) {
        return -1;
    }
    if (detector->config.slow_window_samples == 0u ||
        detector->config.slow_window_samples > DAB_NULL_DETECT_MAX_WINDOW_SAMPLES) {
        return -1;
    }
    if (detector->config.fast_window_samples >= detector->config.slow_window_samples) {
        return -1;
    }
    if (detector->config.enter_ratio_threshold <= 0.0f || detector->config.enter_ratio_threshold >= 1.0f) {
        return -1;
    }
    if (detector->config.exit_ratio_threshold <= 0.0f || detector->config.exit_ratio_threshold >= 1.0f) {
        return -1;
    }
    if (detector->config.enter_ratio_threshold >= detector->config.exit_ratio_threshold) {
        return -1;
    }
    if (detector->config.min_null_samples == 0u) {
        return -1;
    }
    if (detector->config.max_null_samples != 0u &&
        detector->config.max_null_samples < detector->config.min_null_samples) {
        return -1;
    }
    if ((detector->config.expected_interval_samples == 0u) !=
        (detector->config.interval_tolerance_samples == 0u)) {
        return -1;
    }

    dab_null_detect_reset(detector);
    return 0;
}

void dab_null_detect_reset(dab_null_detect_t *detector) {
    if (detector == 0) {
        return;
    }

    detector->fast_power = 1.0f;
    detector->slow_power = 1.0f;
    detector->fast_sum = 0.0f;
    detector->slow_sum = 0.0f;
    detector->fast_inv_window = 1.0f / (float)detector->config.fast_window_samples;
    detector->slow_inv_window = 1.0f / (float)detector->config.slow_window_samples;
    memset(detector->fast_history, 0, sizeof(detector->fast_history));
    memset(detector->slow_history, 0, sizeof(detector->slow_history));
    detector->fast_history_pos = 0u;
    detector->slow_history_pos = 0u;
    detector->fast_history_count = 0u;
    detector->slow_history_count = 0u;
    detector->fast_history_mask = dab_null_detect_is_power_of_two(detector->config.fast_window_samples) ?
        (detector->config.fast_window_samples - 1u) : 0u;
    detector->slow_history_mask = dab_null_detect_is_power_of_two(detector->config.slow_window_samples) ?
        (detector->config.slow_window_samples - 1u) : 0u;
    detector->next_sample_index = 0u;
    detector->run_start_index = 0u;
    detector->run_power_sum = 0.0f;
    detector->run_min_ratio = 1.0f;
    detector->run_length = 0u;
    detector->pending_gap_length = 0u;
    detector->pending_gap_power_sum = 0.0f;
    detector->block_peak_count = 0u;
    detector->last_null_sample_index = 0u;
    detector->has_last_null = 0u;
}

static float dab_null_detect_update_average(float *history,
                                            uint32_t *history_pos,
                                            uint32_t *history_count,
                                            uint32_t window_samples,
                                            uint32_t history_mask,
                                            float inv_window,
                                            float *sum,
                                            float sample) {
    if (*history_count == window_samples) {
        *sum -= history[*history_pos];
    } else {
        (*history_count)++;
    }

    history[*history_pos] = sample;
    *sum += sample;
    if (history_mask != 0u) {
        *history_pos = (*history_pos + 1u) & history_mask;
    } else {
        *history_pos = (*history_pos + 1u) % window_samples;
    }

    if (*history_count == window_samples) {
        return *sum * inv_window;
    }
    return *sum / (float)(*history_count);
}

static int dab_null_detect_run_matches_length(const dab_null_detect_t *detector) {
    if (detector->run_length < detector->config.min_null_samples) {
        return 0;
    }
    if (detector->config.max_null_samples != 0u &&
        detector->run_length > detector->config.max_null_samples) {
        return 0;
    }
    return 1;
}

static int dab_null_detect_run_matches_startup(const dab_null_detect_t *detector) {
    return detector->run_start_index >= detector->config.startup_ignore_samples;
}

static int dab_null_detect_run_matches_cadence(const dab_null_detect_t *detector) {
    uint64_t delta;
    uint64_t expected;
    uint64_t tolerance;

    if (detector->config.expected_interval_samples == 0u) {
        return 1;
    }
    if (!detector->has_last_null) {
        return 1;
    }

    delta = detector->run_start_index - detector->last_null_sample_index;
    expected = detector->config.expected_interval_samples;
    tolerance = detector->config.interval_tolerance_samples;

    return delta >= (expected - tolerance) && delta <= (expected + tolerance);
}

static int dab_null_detect_finalize_run(dab_null_detect_t *detector, dab_null_event_t *event) {
    if (dab_null_detect_run_matches_length(detector) &&
        dab_null_detect_run_matches_startup(detector) &&
        dab_null_detect_run_matches_cadence(detector)) {
        event->sample_index = detector->run_start_index;
        event->length = detector->run_length;
        event->avg_power = detector->run_power_sum / (float)detector->run_length;
        event->min_ratio = detector->run_min_ratio;
        detector->last_null_sample_index = detector->run_start_index;
        detector->has_last_null = 1u;
        detector->run_length = 0u;
        detector->run_power_sum = 0.0f;
        detector->run_min_ratio = 1.0f;
        detector->pending_gap_length = 0u;
        detector->pending_gap_power_sum = 0.0f;
        return 1;
    }

    detector->run_length = 0u;
    detector->run_power_sum = 0.0f;
    detector->run_min_ratio = 1.0f;
    detector->pending_gap_length = 0u;
    detector->pending_gap_power_sum = 0.0f;
    return 0;
}

static void dab_null_detect_discard_run(dab_null_detect_t *detector) {
    detector->run_length = 0u;
    detector->run_power_sum = 0.0f;
    detector->run_min_ratio = 1.0f;
    detector->pending_gap_length = 0u;
    detector->pending_gap_power_sum = 0.0f;
}

static int dab_null_detect_process_power(dab_null_detect_t *detector,
                                         uint64_t block_sample_index,
                                         uint32_t block_sample_count,
                                         dab_null_event_t *event,
                                         float (*next_power)(void *ctx, uint32_t index, uint32_t count),
                                         void *ctx) {
    uint32_t i;
    const uint32_t step = detector->config.sample_decimation;

    event->sample_index = 0u;
    event->length = 0u;
    event->avg_power = 0.0f;
    event->min_ratio = 0.0f;

    detector->next_sample_index = block_sample_index;

    for (i = 0u; i < block_sample_count; i += step) {
        const uint32_t group_count = (block_sample_count - i) < step ? (block_sample_count - i) : step;
        const float power = next_power(ctx, i, group_count);
        const float ratio_fast = dab_null_detect_update_average(detector->fast_history,
                                                                &detector->fast_history_pos,
                                                                &detector->fast_history_count,
                                                                detector->config.fast_window_samples,
                                                                detector->fast_history_mask,
                                                                detector->fast_inv_window,
                                                                &detector->fast_sum,
                                                                power);
        const float ratio_slow = dab_null_detect_update_average(detector->slow_history,
                                                                &detector->slow_history_pos,
                                                                &detector->slow_history_count,
                                                                detector->config.slow_window_samples,
                                                                detector->slow_history_mask,
                                                                detector->slow_inv_window,
                                                                &detector->slow_sum,
                                                                power);
        const float ratio = ratio_slow > 1e-12f ? (ratio_fast / ratio_slow) : 1.0f;

        detector->fast_power = ratio_fast;
        detector->slow_power = ratio_slow;

        if (detector->run_length == 0u) {
            if (ratio < detector->config.enter_ratio_threshold) {
                detector->run_start_index = block_sample_index + i;
                detector->run_min_ratio = ratio;
                detector->run_length = group_count;
                detector->run_power_sum = power * (float)group_count;
                detector->pending_gap_length = 0u;
                detector->pending_gap_power_sum = 0.0f;
            }
        } else if (ratio < detector->config.exit_ratio_threshold) {
            if (detector->pending_gap_length != 0u) {
                detector->run_length += detector->pending_gap_length;
                detector->run_power_sum += detector->pending_gap_power_sum;
                detector->pending_gap_length = 0u;
                detector->pending_gap_power_sum = 0.0f;
            }
            detector->run_length += group_count;
            detector->run_power_sum += power * (float)group_count;
            if (ratio < detector->run_min_ratio) {
                detector->run_min_ratio = ratio;
            }
            if (detector->config.max_null_samples != 0u &&
                detector->run_length > (detector->config.max_null_samples + detector->config.max_merge_gap_samples)) {
                dab_null_detect_discard_run(detector);
            }
        } else {
            detector->pending_gap_length += group_count;
            detector->pending_gap_power_sum += power * (float)group_count;
            if (detector->pending_gap_length > detector->config.max_merge_gap_samples) {
                if (dab_null_detect_finalize_run(detector, event)) {
                    detector->next_sample_index = block_sample_index + i + group_count;
                    return 1;
                }
            } else if (detector->config.max_null_samples != 0u &&
                       (detector->run_length + detector->pending_gap_length) >
                       (detector->config.max_null_samples + detector->config.max_merge_gap_samples)) {
                dab_null_detect_discard_run(detector);
            }
        }

        if (power > detector->slow_power) {
            detector->block_peak_count += group_count;
        }
    }

    detector->next_sample_index = block_sample_index + block_sample_count;
    return 0;
}

typedef struct {
    const dab_frontend_block_t *block;
} dab_null_detect_f32_ctx_t;

static float dab_null_detect_next_power_f32(void *ctx, uint32_t index, uint32_t count) {
    const dab_null_detect_f32_ctx_t *f32 = (const dab_null_detect_f32_ctx_t *)ctx;
    uint32_t i;
    float sum = 0.0f;

    for (i = 0u; i < count; ++i) {
        const float ii = f32->block->i_data[index + i];
        const float qq = f32->block->q_data[index + i];
        sum += (ii * ii) + (qq * qq);
    }
    return sum / (float)count;
}

typedef struct {
    const dab_sdr_block_view_t *block;
    float power[DAB_IQ_BLOCK_SAMPLES];
} dab_null_detect_i16_ctx_t;

static float dab_null_detect_next_power_i16(void *ctx, uint32_t index, uint32_t count) {
    const dab_null_detect_i16_ctx_t *i16 = (const dab_null_detect_i16_ctx_t *)ctx;
    uint32_t i;
    float sum = 0.0f;

    for (i = 0u; i < count; ++i) {
        sum += i16->power[index + i];
    }
    return sum / (float)count;
}

int dab_null_detect_process(dab_null_detect_t *detector,
                            const dab_frontend_block_t *block,
                            dab_null_event_t *event) {
    dab_null_detect_f32_ctx_t ctx;

    if (detector == 0 || block == 0 || event == 0) {
        return -1;
    }
    if (block->i_data == 0 || block->q_data == 0) {
        return -1;
    }

    ctx.block = block;
    return dab_null_detect_process_power(detector,
                                         block->sample_index,
                                         block->sample_count,
                                         event,
                                         dab_null_detect_next_power_f32,
                                         &ctx);
}

int dab_null_detect_process_i16(dab_null_detect_t *detector,
                                const dab_sdr_block_view_t *block,
                                dab_null_event_t *event) {
    dab_null_detect_i16_ctx_t ctx;

    if (detector == 0 || block == 0 || event == 0) {
        return -1;
    }
    if (block->iq_interleaved == 0) {
        return -1;
    }

    ctx.block = block;
    dab_simd_get_ops()->power_i16_interleaved_f32(ctx.power, block->iq_interleaved, block->sample_count);
    return dab_null_detect_process_power(detector,
                                         block->sample_index,
                                         block->sample_count,
                                         event,
                                         dab_null_detect_next_power_i16,
                                         &ctx);
}
