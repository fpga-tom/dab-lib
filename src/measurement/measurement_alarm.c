#include "dab/measurement_alarm.h"

#include <math.h>
#include <string.h>

static int alarm_is_violating(const dab_measurement_alarm_config_t *config,
                              float value) {
    return config->condition == DAB_MEASUREMENT_ALARM_GREATER_THAN
        ? value > config->threshold
        : value < config->threshold;
}

static int alarm_is_recovered(const dab_measurement_alarm_config_t *config,
                              float value) {
    return config->condition == DAB_MEASUREMENT_ALARM_GREATER_THAN
        ? value <= config->recovery_threshold
        : value >= config->recovery_threshold;
}

void dab_measurement_alarm_configure(dab_measurement_alarm_state_t *state,
                                     const dab_measurement_alarm_config_t *config) {
    if (state == 0 || config == 0) {
        return;
    }
    memset(state, 0, sizeof(*state));
    state->config = *config;
}

void dab_measurement_alarm_reset(dab_measurement_alarm_state_t *state) {
    dab_measurement_alarm_config_t config;
    if (state == 0) {
        return;
    }
    config = state->config;
    memset(state, 0, sizeof(*state));
    state->config = config;
}

dab_measurement_alarm_event_t dab_measurement_alarm_update(
    dab_measurement_alarm_state_t *state,
    uint64_t timestamp_ns,
    int value_valid,
    float value) {
    if (state == 0) {
        return DAB_MEASUREMENT_ALARM_NO_EVENT;
    }

    if (state->last_timestamp_ns != 0u && timestamp_ns < state->last_timestamp_ns) {
        state->pending = 0;
        state->pending_since_ns = 0u;
    }
    state->last_timestamp_ns = timestamp_ns;

    if (!value_valid || !isfinite(value)) {
        state->pending = 0;
        state->pending_since_ns = 0u;
        return DAB_MEASUREMENT_ALARM_NO_EVENT;
    }

    if (state->active) {
        if (alarm_is_recovered(&state->config, value)) {
            state->active = 0;
            state->pending = 0;
            state->pending_since_ns = 0u;
            return DAB_MEASUREMENT_ALARM_RECOVERED;
        }
        return DAB_MEASUREMENT_ALARM_NO_EVENT;
    }

    if (!alarm_is_violating(&state->config, value)) {
        state->pending = 0;
        state->pending_since_ns = 0u;
        return DAB_MEASUREMENT_ALARM_NO_EVENT;
    }

    if (!state->pending) {
        state->pending = 1;
        state->pending_since_ns = timestamp_ns;
    }
    if (timestamp_ns - state->pending_since_ns >= state->config.minimum_duration_ns) {
        state->active = 1;
        state->pending = 0;
        state->pending_since_ns = 0u;
        return DAB_MEASUREMENT_ALARM_TRIGGERED;
    }
    return DAB_MEASUREMENT_ALARM_NO_EVENT;
}
