#ifndef DAB_HAL_RTLSDR_ADAPTER_H
#define DAB_HAL_RTLSDR_ADAPTER_H

#include <stdint.h>

#include "dab/config.h"
#include "dab/hal/rtlsdr_device.h"
#include "dab/hal/sdr_device.h"
#include "rtlsdr_api.h"

typedef void (*dab_rtlsdr_samples_cb_t)(const int16_t *i_samples,
                                        const int16_t *q_samples,
                                        uint32_t sample_count,
                                        uint64_t sample_index,
                                        uint64_t timestamp_ns,
                                        void *user);

typedef struct dab_rtlsdr_adapter dab_rtlsdr_adapter_t;

typedef struct {
    dab_sdr_status_t (*start)(dab_rtlsdr_adapter_t *adapter,
                              dab_rtlsdr_samples_cb_t callback,
                              void *user);
    dab_sdr_status_t (*stop)(dab_rtlsdr_adapter_t *adapter);
    void (*destroy)(dab_rtlsdr_adapter_t *adapter);
} dab_rtlsdr_adapter_vtbl_t;

struct dab_rtlsdr_adapter {
    const dab_rtlsdr_adapter_vtbl_t *vtbl;
    dab_stream_config_t config;
    dab_rtlsdr_config_t device_config;
    uint32_t device_index;
    int uses_real_api;
};

dab_rtlsdr_adapter_t *dab_rtlsdr_adapter_create(const dab_stream_config_t *config,
                                                const dab_rtlsdr_config_t *device_config,
                                                uint32_t device_index);
dab_rtlsdr_adapter_t *dab_rtlsdr_stub_adapter_create(const dab_stream_config_t *config,
                                                     const dab_rtlsdr_config_t *device_config,
                                                     uint32_t device_index);
dab_rtlsdr_adapter_t *dab_rtlsdr_real_adapter_create(const dab_stream_config_t *config,
                                                     const dab_rtlsdr_config_t *device_config,
                                                     uint32_t device_index,
                                                     const dab_rtlsdr_api_t *api);

#endif
