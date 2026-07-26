#ifndef DAB_HAL_SDRPLAY_ADAPTER_H
#define DAB_HAL_SDRPLAY_ADAPTER_H

#include <stdint.h>

#include "dab/config.h"
#include "dab/hal/sdr_device.h"
#include "dab/hal/sdrplay_device.h"
#include "dab/os/os_mutex.h"
#include "sdrplay_api.h"

typedef void (*dab_sdrplay_samples_cb_t)(const int16_t *i_samples,
                                         const int16_t *q_samples,
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
    dab_sdrplay_config_t device_config;
    dab_mutex_t gain_diagnostics_mutex;
    dab_sdrplay_gain_diagnostics_t gain_diagnostics;
    int gain_diagnostics_mutex_ready;
    int uses_real_api;
};

int dab_sdrplay_adapter_init_base(dab_sdrplay_adapter_t *adapter,
                                  const dab_stream_config_t *config,
                                  const dab_sdrplay_config_t *device_config);
void dab_sdrplay_adapter_destroy_base(dab_sdrplay_adapter_t *adapter);
void dab_sdrplay_adapter_publish_gain_diagnostics(
    dab_sdrplay_adapter_t *adapter,
    const dab_sdrplay_gain_diagnostics_t *diagnostics);
dab_sdr_status_t dab_sdrplay_adapter_get_gain_diagnostics(
    dab_sdrplay_adapter_t *adapter,
    dab_sdrplay_gain_diagnostics_t *diagnostics);
dab_sdrplay_adapter_t *dab_sdrplay_adapter_create(const dab_stream_config_t *config,
                                                  const dab_sdrplay_config_t *device_config);
dab_sdrplay_adapter_t *dab_sdrplay_stub_adapter_create(const dab_stream_config_t *config,
                                                       const dab_sdrplay_config_t *device_config);
dab_sdrplay_adapter_t *dab_sdrplay_real_adapter_create(const dab_stream_config_t *config,
                                                       const dab_sdrplay_config_t *device_config,
                                                       const dab_sdrplay_api_t *api);

#endif
