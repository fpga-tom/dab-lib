#ifndef DAB_HAL_RTLSDR_DEVICE_H
#define DAB_HAL_RTLSDR_DEVICE_H

#include "dab/hal/sdr_device.h"

typedef enum {
    DAB_RTLSDR_GAIN_MODE_AUTO = 0,
    DAB_RTLSDR_GAIN_MODE_MANUAL = 1
} dab_rtlsdr_gain_mode_t;

typedef struct {
    dab_rtlsdr_gain_mode_t gain_mode;
    int ppm_correction;
    int bias_tee_enabled;
} dab_rtlsdr_config_t;

void dab_rtlsdr_default_config(dab_rtlsdr_config_t *config);
dab_sdr_status_t dab_rtlsdr_configure_device(dab_sdr_device_t *device, const dab_rtlsdr_config_t *config);
uint32_t dab_rtlsdr_stream_dropped_blocks(const dab_sdr_stream_t *stream);

#endif
