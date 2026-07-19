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
