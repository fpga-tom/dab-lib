#ifndef DAB_MEASUREMENT_ALARM_H
#define DAB_MEASUREMENT_ALARM_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    DAB_MEASUREMENT_ALARM_LESS_THAN = 0,
    DAB_MEASUREMENT_ALARM_GREATER_THAN = 1
} dab_measurement_alarm_condition_t;

typedef enum {
    DAB_MEASUREMENT_ALARM_NO_EVENT = 0,
    DAB_MEASUREMENT_ALARM_TRIGGERED = 1,
    DAB_MEASUREMENT_ALARM_RECOVERED = 2
} dab_measurement_alarm_event_t;

typedef struct {
    dab_measurement_alarm_condition_t condition;
    float threshold;
    float recovery_threshold;
    uint64_t minimum_duration_ns;
} dab_measurement_alarm_config_t;

typedef struct {
    dab_measurement_alarm_config_t config;
    uint64_t pending_since_ns;
    uint64_t last_timestamp_ns;
    int pending;
    int active;
} dab_measurement_alarm_state_t;

void dab_measurement_alarm_configure(dab_measurement_alarm_state_t *state,
                                     const dab_measurement_alarm_config_t *config);
void dab_measurement_alarm_reset(dab_measurement_alarm_state_t *state);
dab_measurement_alarm_event_t dab_measurement_alarm_update(
    dab_measurement_alarm_state_t *state,
    uint64_t timestamp_ns,
    int value_valid,
    float value);

#ifdef __cplusplus
}
#endif

#endif
