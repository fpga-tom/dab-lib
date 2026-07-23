#include "dab/scan.h"

#include "test_support.h"

void test_scan_config_normalizes_timing(void) {
    dab_scan_config_t config;

    dab_scan_default_config(&config);
    TEST_ASSERT(config.per_frequency_seconds == DAB_SCAN_DEFAULT_PER_FREQUENCY_SECONDS);
    TEST_ASSERT(config.label_wait_seconds == DAB_SCAN_DEFAULT_LABEL_WAIT_SECONDS);
    TEST_ASSERT(config.hard_channel_seconds == DAB_SCAN_DEFAULT_HARD_CHANNEL_SECONDS);

    config.per_frequency_seconds = 0u;
    config.label_wait_seconds = 0u;
    config.hard_channel_seconds = 0u;
    dab_scan_normalize_config(&config);
    TEST_ASSERT(config.per_frequency_seconds == DAB_SCAN_DEFAULT_PER_FREQUENCY_SECONDS);
    TEST_ASSERT(config.label_wait_seconds == DAB_SCAN_DEFAULT_LABEL_WAIT_SECONDS);
    TEST_ASSERT(config.hard_channel_seconds == DAB_SCAN_DEFAULT_HARD_CHANNEL_SECONDS);

    config.per_frequency_seconds = 100u;
    config.label_wait_seconds = 1u;
    config.hard_channel_seconds = 10u;
    dab_scan_normalize_config(&config);
    TEST_ASSERT(config.per_frequency_seconds == DAB_SCAN_MAX_PER_FREQUENCY_SECONDS);
    TEST_ASSERT(config.label_wait_seconds == DAB_SCAN_MIN_LABEL_WAIT_SECONDS);
    TEST_ASSERT(config.hard_channel_seconds ==
                DAB_SCAN_MAX_PER_FREQUENCY_SECONDS + DAB_SCAN_MIN_LABEL_WAIT_SECONDS);

    config.hard_channel_seconds = 999u;
    dab_scan_normalize_config(&config);
    TEST_ASSERT(config.hard_channel_seconds == DAB_SCAN_MAX_HARD_CHANNEL_SECONDS);
}
