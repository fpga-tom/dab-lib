#include "dab/core/null_detect.h"

#include <string.h>
#include <stdio.h>

static uint32_t dab_null_detect_alpha_to_window(float alpha) {
    uint32_t window;

    if (alpha <= 0.0f || alpha >= 1.0f) {
        return 0u;
    }

    window = (uint32_t)((2.0f / alpha) - 1.0f + 0.5f);
    return window < 1u ? 1u : window;
}

void dab_null_detect_default_config(dab_null_detect_config_t *config) {
    if (config == 0) {
        return;
    }
    config->fast_alpha = 0.05f;
    config->slow_alpha = 0.001f;
    config->fast_window_samples = 0u;
    config->slow_window_samples = 0u;
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
    memset(detector->fast_history, 0, sizeof(detector->fast_history));
    memset(detector->slow_history, 0, sizeof(detector->slow_history));
    detector->fast_history_pos = 0u;
    detector->slow_history_pos = 0u;
    detector->fast_history_count = 0u;
    detector->slow_history_count = 0u;
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
                                            float *sum,
                                            float sample) {
    if (*history_count == window_samples) {
        *sum -= history[*history_pos];
    } else {
        (*history_count)++;
    }

    history[*history_pos] = sample;
    *sum += sample;
    *history_pos = (*history_pos + 1u) % window_samples;

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

int dab_null_detect_process(dab_null_detect_t *detector,
                            const dab_frontend_block_t *block,
                            dab_null_event_t *event) {
    uint32_t i;

    if (detector == 0 || block == 0 || event == 0) {
        return -1;
    }
    if (block->i_data == 0 || block->q_data == 0) {
        return -1;
    }

    event->sample_index = 0u;
    event->length = 0u;
    event->avg_power = 0.0f;
    event->min_ratio = 0.0f;

    detector->next_sample_index = block->sample_index;

    for (i = 0u; i < block->sample_count; ++i) {
        const float ii = block->i_data[i];
        const float qq = block->q_data[i];
        const float power = (ii * ii) + (qq * qq);
        float ratio;

        detector->fast_power = dab_null_detect_update_average(detector->fast_history,
                                                              &detector->fast_history_pos,
                                                              &detector->fast_history_count,
                                                              detector->config.fast_window_samples,
                                                              &detector->fast_sum,
                                                              power);
        detector->slow_power = dab_null_detect_update_average(detector->slow_history,
                                                              &detector->slow_history_pos,
                                                              &detector->slow_history_count,
                                                              detector->config.slow_window_samples,
                                                              &detector->slow_sum,
                                                              power);

        ratio = detector->slow_power > 1e-12f ? (detector->fast_power / detector->slow_power) : 1.0f;
//	static uint64_t counter = 0;
//	fprintf(stderr, "%llu %f\n", counter++, ratio);

        if (detector->run_length == 0u) {
            if (ratio < detector->config.enter_ratio_threshold) {
                detector->run_start_index = block->sample_index + i;
                detector->run_min_ratio = ratio;
                detector->run_length = 1u;
                detector->run_power_sum = power;
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
            detector->run_length++;
            detector->run_power_sum += power;
            if (ratio < detector->run_min_ratio) {
                detector->run_min_ratio = ratio;
            }
            if (detector->config.max_null_samples != 0u &&
                detector->run_length > (detector->config.max_null_samples + detector->config.max_merge_gap_samples)) {
                dab_null_detect_discard_run(detector);
            }
        } else {
            detector->pending_gap_length++;
            detector->pending_gap_power_sum += power;
            if (detector->pending_gap_length > detector->config.max_merge_gap_samples) {
                if (dab_null_detect_finalize_run(detector, event)) {
                    detector->next_sample_index = block->sample_index + i + 1u;
                    return 1;
                }
            } else if (detector->config.max_null_samples != 0u &&
                       (detector->run_length + detector->pending_gap_length) >
                       (detector->config.max_null_samples + detector->config.max_merge_gap_samples)) {
                dab_null_detect_discard_run(detector);
            }
        }

        if (power > detector->slow_power) {
            detector->block_peak_count++;
        }
    }

    detector->next_sample_index = block->sample_index + block->sample_count;
    return 0;
}
