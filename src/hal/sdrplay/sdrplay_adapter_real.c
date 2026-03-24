#include "sdrplay_adapter.h"

#include <stdlib.h>

typedef struct {
    dab_sdrplay_adapter_t base;
    dab_sdrplay_api_t api;
    dab_sdrplay_samples_cb_t callback;
    void *user;
    float api_version_value;
    int opened;
    int started;
} dab_sdrplay_real_adapter_t;

static dab_sdr_status_t dab_sdrplay_real_start(dab_sdrplay_adapter_t *base,
                                               dab_sdrplay_samples_cb_t callback,
                                               void *user) {
    dab_sdrplay_real_adapter_t *adapter = (dab_sdrplay_real_adapter_t *)base;

    if (adapter == 0 || callback == 0) {
        return DAB_SDR_STATUS_INVALID_ARGUMENT;
    }
    if (adapter->started) {
        return DAB_SDR_STATUS_BUSY;
    }
    if (adapter->api.open_api() != 0) {
        return DAB_SDR_STATUS_ERROR;
    }
    adapter->opened = 1;
    adapter->callback = callback;
    adapter->user = user;
    if (adapter->api.api_version != 0) {
        (void)adapter->api.api_version(&adapter->api_version_value);
    }
    adapter->started = 1;
    return DAB_SDR_STATUS_NOT_IMPLEMENTED;
}

static dab_sdr_status_t dab_sdrplay_real_stop(dab_sdrplay_adapter_t *base) {
    dab_sdrplay_real_adapter_t *adapter = (dab_sdrplay_real_adapter_t *)base;

    if (adapter == 0) {
        return DAB_SDR_STATUS_INVALID_ARGUMENT;
    }
    if (!adapter->opened) {
        adapter->started = 0;
        return DAB_SDR_STATUS_OK;
    }
    if (adapter->api.close_api() != 0) {
        return DAB_SDR_STATUS_ERROR;
    }
    adapter->opened = 0;
    adapter->started = 0;
    return DAB_SDR_STATUS_OK;
}

static void dab_sdrplay_real_destroy(dab_sdrplay_adapter_t *base) {
    dab_sdrplay_real_adapter_t *adapter = (dab_sdrplay_real_adapter_t *)base;

    if (adapter == 0) {
        return;
    }
    if (adapter->opened) {
        (void)dab_sdrplay_real_stop(base);
    }
    dab_sdrplay_api_unload(&adapter->api);
    free(adapter);
}

static const dab_sdrplay_adapter_vtbl_t DAB_SDRPLAY_REAL_VTBL = {
    dab_sdrplay_real_start,
    dab_sdrplay_real_stop,
    dab_sdrplay_real_destroy
};

dab_sdrplay_adapter_t *dab_sdrplay_real_adapter_create(const dab_stream_config_t *config,
                                                       const dab_sdrplay_config_t *device_config,
                                                       const dab_sdrplay_api_t *api) {
    dab_sdrplay_real_adapter_t *adapter;

    if (config == 0 || device_config == 0 || api == 0 || !dab_sdrplay_api_is_usable(api)) {
        return 0;
    }

    adapter = (dab_sdrplay_real_adapter_t *)calloc(1u, sizeof(*adapter));
    if (adapter == 0) {
        return 0;
    }

    adapter->base.vtbl = &DAB_SDRPLAY_REAL_VTBL;
    adapter->base.config = *config;
    adapter->base.device_config = *device_config;
    adapter->base.uses_real_api = 1;
    adapter->api = *api;
    return &adapter->base;
}
