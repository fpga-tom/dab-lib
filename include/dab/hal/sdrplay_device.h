#ifndef DAB_HAL_SDRPLAY_DEVICE_H
#define DAB_HAL_SDRPLAY_DEVICE_H

#include <stdint.h>

#include "dab/hal/sdr_device.h"

typedef enum {
    DAB_SDRPLAY_GAIN_MODE_AUTO = 0,
    DAB_SDRPLAY_GAIN_MODE_MANUAL = 1
} dab_sdrplay_gain_mode_t;

typedef enum {
    DAB_SDRPLAY_IF_MODE_ZERO_IF = 0,
    DAB_SDRPLAY_IF_MODE_LOW_IF = 1
} dab_sdrplay_if_mode_t;

typedef enum {
    DAB_SDRPLAY_ANTENNA_AUTO = 0,
    DAB_SDRPLAY_ANTENNA_A = 1,
    DAB_SDRPLAY_ANTENNA_B = 2
} dab_sdrplay_antenna_t;

typedef struct {
    dab_sdrplay_gain_mode_t gain_mode;
    dab_sdrplay_if_mode_t if_mode;
    dab_sdrplay_antenna_t antenna;
    uint8_t lna_state;
    int if_gain_db;
    int bias_tee_enabled;
    int ppm_correction;
} dab_sdrplay_config_t;

void dab_sdrplay_default_config(dab_sdrplay_config_t *config);
dab_sdr_status_t dab_sdrplay_configure_device(dab_sdr_device_t *device, const dab_sdrplay_config_t *config);

#endif
