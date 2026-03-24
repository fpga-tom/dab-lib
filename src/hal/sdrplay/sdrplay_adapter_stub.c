#include "sdrplay_adapter.h"

#include <stdlib.h>
#include <string.h>

#include "dab/os/os_thread.h"
#include "dab/os/os_time.h"
#include "sdrplay_api.h"

enum {
    DAB_SDRPLAY_STUB_SAMPLES_PER_CALLBACK = 1024
};

typedef struct {
    dab_sdrplay_adapter_t base;
    dab_sdrplay_api_t api;
    dab_thread_t thread;
    dab_sdrplay_samples_cb_t callback;
    void *user;
    uint64_t sample_index;
    int running;
    int stop_requested;
} dab_sdrplay_stub_adapter_t;

static void dab_sdrplay_stub_fill_iq(int16_t *dst, uint32_t sample_count, uint64_t sample_index) {
    uint32_t i;
    for (i = 0u; i < sample_count; ++i) {
        int16_t value = (int16_t)((sample_index + i) & 0x7fff);
        dst[2u * i] = value;
        dst[(2u * i) + 1u] = (int16_t)(-value);
    }
}

static void dab_sdrplay_stub_apply_config(int16_t *dst,
                                          uint32_t sample_count,
                                          const dab_sdrplay_config_t *config) {
    uint32_t i;
    int16_t dc_offset;

    if (config == NULL) {
        return;
    }

    dc_offset = (int16_t)(config->lna_state * 16u);
    if (config->gain_mode == DAB_SDRPLAY_GAIN_MODE_MANUAL) {
        dc_offset = (int16_t)(dc_offset + (config->if_gain_db * 4));
    }
    if (config->antenna == DAB_SDRPLAY_ANTENNA_B) {
        dc_offset = (int16_t)(dc_offset + 3);
    }
    if (config->bias_tee_enabled) {
        dc_offset = (int16_t)(dc_offset + 1);
    }

    for (i = 0u; i < sample_count; ++i) {
        dst[2u * i] = (int16_t)(dst[2u * i] + dc_offset);
        if (config->if_mode == DAB_SDRPLAY_IF_MODE_LOW_IF) {
            dst[(2u * i) + 1u] = (int16_t)(-dst[(2u * i) + 1u]);
        }
    }
}

static int dab_sdrplay_stub_thread(void *arg) {
    dab_sdrplay_stub_adapter_t *adapter = (dab_sdrplay_stub_adapter_t *)arg;
    int16_t iq[2u * DAB_SDRPLAY_STUB_SAMPLES_PER_CALLBACK];

    while (!adapter->stop_requested) {
        dab_sdrplay_stub_fill_iq(iq, DAB_SDRPLAY_STUB_SAMPLES_PER_CALLBACK, adapter->sample_index);
        dab_sdrplay_stub_apply_config(iq,
                                      DAB_SDRPLAY_STUB_SAMPLES_PER_CALLBACK,
                                      &adapter->base.device_config);
        if (adapter->callback != NULL) {
            adapter->callback(iq,
                              DAB_SDRPLAY_STUB_SAMPLES_PER_CALLBACK,
                              adapter->sample_index,
                              dab_time_monotonic_ns(),
                              adapter->user);
        }
        adapter->sample_index += DAB_SDRPLAY_STUB_SAMPLES_PER_CALLBACK;
    }

    adapter->running = 0;
    return 0;
}

static dab_sdr_status_t dab_sdrplay_stub_start(dab_sdrplay_adapter_t *base,
                                               dab_sdrplay_samples_cb_t callback,
                                               void *user) {
    dab_sdrplay_stub_adapter_t *adapter = (dab_sdrplay_stub_adapter_t *)base;

    if (adapter == NULL || callback == NULL) {
        return DAB_SDR_STATUS_INVALID_ARGUMENT;
    }
    if (adapter->running) {
        return DAB_SDR_STATUS_BUSY;
    }

    adapter->callback = callback;
    adapter->user = user;
    adapter->stop_requested = 0;
    adapter->running = 1;
    if (dab_thread_create(&adapter->thread, dab_sdrplay_stub_thread, adapter, "sdrplay-stub") != 0) {
        adapter->running = 0;
        return DAB_SDR_STATUS_ERROR;
    }

    return DAB_SDR_STATUS_OK;
}

static dab_sdr_status_t dab_sdrplay_stub_stop(dab_sdrplay_adapter_t *base) {
    dab_sdrplay_stub_adapter_t *adapter = (dab_sdrplay_stub_adapter_t *)base;
    int rc = 0;

    if (adapter == NULL) {
        return DAB_SDR_STATUS_INVALID_ARGUMENT;
    }
    if (!adapter->running) {
        return DAB_SDR_STATUS_OK;
    }

    adapter->stop_requested = 1;
    if (dab_thread_join(&adapter->thread, &rc) != 0) {
        adapter->running = 0;
        return DAB_SDR_STATUS_ERROR;
    }

    adapter->running = 0;
    return rc == 0 ? DAB_SDR_STATUS_OK : DAB_SDR_STATUS_ERROR;
}

static void dab_sdrplay_stub_destroy(dab_sdrplay_adapter_t *base) {
    dab_sdrplay_stub_adapter_t *adapter = (dab_sdrplay_stub_adapter_t *)base;
    if (adapter == NULL) {
        return;
    }
    if (adapter->running) {
        (void)dab_sdrplay_stub_stop(base);
    }
    dab_sdrplay_api_unload(&adapter->api);
    free(adapter);
}

static const dab_sdrplay_adapter_vtbl_t DAB_SDRPLAY_STUB_VTBL = {
    dab_sdrplay_stub_start,
    dab_sdrplay_stub_stop,
    dab_sdrplay_stub_destroy
};

dab_sdrplay_adapter_t *dab_sdrplay_adapter_create(const dab_stream_config_t *config,
                                                  const dab_sdrplay_config_t *device_config) {
    dab_sdrplay_stub_adapter_t *adapter;

    if (config == NULL || device_config == NULL) {
        return NULL;
    }

    adapter = (dab_sdrplay_stub_adapter_t *)calloc(1u, sizeof(*adapter));
    if (adapter == NULL) {
        return NULL;
    }

    adapter->base.vtbl = &DAB_SDRPLAY_STUB_VTBL;
    adapter->base.config = *config;
    adapter->base.device_config = *device_config;
    if (dab_sdrplay_api_try_load(&adapter->api) == 0 &&
        adapter->api.has_open_api &&
        adapter->api.has_close_api) {
        adapter->base.uses_real_api = 1;
    }
    return &adapter->base;
}
