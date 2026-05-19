#include "dab/receive.h"

#include <string.h>

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
}

#define DAB_RECEIVE_NO_MAIN 1
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-function"
#pragma clang diagnostic ignored "-Wunused-but-set-variable"
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wunused-but-set-variable"
#endif
#include "../../tools/dab_sdrplay_audio.c"
#if defined(__clang__)
#pragma clang diagnostic pop
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
