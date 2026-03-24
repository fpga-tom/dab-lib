#ifndef DAB_HAL_SDRPLAY_ADAPTER_H
#define DAB_HAL_SDRPLAY_ADAPTER_H

#include <stdint.h>

#include "dab/config.h"
#include "dab/hal/sdr_device.h"

typedef void (*dab_sdrplay_samples_cb_t)(const int16_t *iq_interleaved,
                                         uint32_t sample_count,
                                         uint64_t sample_index,
                                         uint64_t timestamp_ns,
                                         void *user);

typedef struct dab_sdrplay_adapter dab_sdrplay_adapter_t;

typedef struct {
    dab_sdr_status_t (*start)(dab_sdrplay_adapter_t *adapter,
                              dab_sdrplay_samples_cb_t callback,
                              void *user);
    dab_sdr_status_t (*stop)(dab_sdrplay_adapter_t *adapter);
    void (*destroy)(dab_sdrplay_adapter_t *adapter);
} dab_sdrplay_adapter_vtbl_t;

struct dab_sdrplay_adapter {
    const dab_sdrplay_adapter_vtbl_t *vtbl;
    dab_stream_config_t config;
};

dab_sdrplay_adapter_t *dab_sdrplay_adapter_create(const dab_stream_config_t *config);

#endif
