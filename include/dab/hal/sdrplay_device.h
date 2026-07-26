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

#define DAB_SDRPLAY_SERIAL_CAPACITY 64

typedef struct {
    int valid;
    int applied;
    int agc_enabled;
    int gain_reduction_db;
    uint8_t lna_state;
    int system_gain_valid;
    float system_gain_db;
    uint64_t applied_sample_index;
    uint32_t change_generation;
    uint8_t hardware_version;
    float api_version;
    char serial[DAB_SDRPLAY_SERIAL_CAPACITY];
} dab_sdrplay_gain_diagnostics_t;

void dab_sdrplay_default_config(dab_sdrplay_config_t *config);
dab_sdr_status_t dab_sdrplay_configure_device(dab_sdr_device_t *device, const dab_sdrplay_config_t *config);
uint32_t dab_sdrplay_stream_dropped_blocks(const dab_sdr_stream_t *stream);
dab_sdr_status_t dab_sdrplay_stream_get_gain_diagnostics(
    dab_sdr_stream_t *stream,
    dab_sdrplay_gain_diagnostics_t *diagnostics);

#endif
