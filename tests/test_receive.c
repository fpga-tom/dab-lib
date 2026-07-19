#include "dab/receive.h"

#include <string.h>

#include "test_support.h"

void test_receive_default_config_uses_rtlsdr_playback_defaults(void) {
    dab_receive_config_t config;

    memset(&config, 0xA5, sizeof(config));
    dab_receive_default_config(&config);

    TEST_ASSERT(config.driver != 0);
    TEST_ASSERT(strcmp(config.driver, "rtlsdr") == 0);
    TEST_ASSERT(config.gain_db == 40);
    TEST_ASSERT(config.agc_enabled == 1);
    TEST_ASSERT(config.android_usb_fd == -1);
    TEST_ASSERT(config.use_real_device == 1);
}

void test_receive_service_rejects_incomplete_config_before_opening_device(void) {
    dab_receive_config_t config;

    dab_receive_default_config(&config);
    TEST_ASSERT(dab_receive_service(&config, 0, 0) == 2);

    config.frequency_hz = 220352000u;
    TEST_ASSERT(dab_receive_service(&config, 0, 0) == 2);
}
