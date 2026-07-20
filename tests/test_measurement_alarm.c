#include "dab/measurement_alarm.h"

#include <stdio.h>
#include <stdlib.h>

#define TEST_ASSERT(condition)                                                     \
    do {                                                                           \
        if (!(condition)) {                                                        \
            fprintf(stderr, "assertion failed: %s (%s:%d)\n",                    \
                    #condition, __FILE__, __LINE__);                               \
            exit(EXIT_FAILURE);                                                    \
        }                                                                          \
    } while (0)

void test_measurement_alarm_requires_duration_and_recovery_hysteresis(void) {
    dab_measurement_alarm_state_t state;
    dab_measurement_alarm_config_t config;

    config.condition = DAB_MEASUREMENT_ALARM_LESS_THAN;
    config.threshold = 12.0f;
    config.recovery_threshold = 14.0f;
    config.minimum_duration_ns = 3000000000ull;
    dab_measurement_alarm_configure(&state, &config);

    TEST_ASSERT(dab_measurement_alarm_update(&state, 1000000000ull, 1, 11.8f) ==
                DAB_MEASUREMENT_ALARM_NO_EVENT);
    TEST_ASSERT(dab_measurement_alarm_update(&state, 3500000000ull, 1, 11.5f) ==
                DAB_MEASUREMENT_ALARM_NO_EVENT);
    TEST_ASSERT(dab_measurement_alarm_update(&state, 4000000000ull, 1, 11.0f) ==
                DAB_MEASUREMENT_ALARM_TRIGGERED);
    TEST_ASSERT(state.active != 0);
    TEST_ASSERT(dab_measurement_alarm_update(&state, 5000000000ull, 1, 12.5f) ==
                DAB_MEASUREMENT_ALARM_NO_EVENT);
    TEST_ASSERT(state.active != 0);
    TEST_ASSERT(dab_measurement_alarm_update(&state, 6000000000ull, 1, 14.0f) ==
                DAB_MEASUREMENT_ALARM_RECOVERED);
    TEST_ASSERT(state.active == 0);
}

void test_measurement_alarm_short_violation_does_not_accumulate(void) {
    dab_measurement_alarm_state_t state;
    dab_measurement_alarm_config_t config;

    config.condition = DAB_MEASUREMENT_ALARM_GREATER_THAN;
    config.threshold = 1.0f;
    config.recovery_threshold = 0.25f;
    config.minimum_duration_ns = 2000000000ull;
    dab_measurement_alarm_configure(&state, &config);

    TEST_ASSERT(dab_measurement_alarm_update(&state, 1000000000ull, 1, 1.5f) ==
                DAB_MEASUREMENT_ALARM_NO_EVENT);
    TEST_ASSERT(dab_measurement_alarm_update(&state, 2000000000ull, 1, 0.8f) ==
                DAB_MEASUREMENT_ALARM_NO_EVENT);
    TEST_ASSERT(dab_measurement_alarm_update(&state, 3000000000ull, 1, 1.5f) ==
                DAB_MEASUREMENT_ALARM_NO_EVENT);
    TEST_ASSERT(dab_measurement_alarm_update(&state, 5000000000ull, 1, 1.5f) ==
                DAB_MEASUREMENT_ALARM_TRIGGERED);
    TEST_ASSERT(dab_measurement_alarm_update(&state, 6000000000ull, 0, 0.0f) ==
                DAB_MEASUREMENT_ALARM_NO_EVENT);
    TEST_ASSERT(state.active != 0);
    TEST_ASSERT(dab_measurement_alarm_update(&state, 7000000000ull, 1, 0.25f) ==
                DAB_MEASUREMENT_ALARM_RECOVERED);
}
