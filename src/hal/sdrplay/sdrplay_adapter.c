#include "sdrplay_adapter.h"

#include <stdlib.h>
#include <string.h>

int dab_sdrplay_adapter_init_base(dab_sdrplay_adapter_t *adapter,
                                  const dab_stream_config_t *config,
                                  const dab_sdrplay_config_t *device_config) {
    if (adapter == 0 || config == 0 || device_config == 0) {
        return -1;
    }
    adapter->config = *config;
    adapter->device_config = *device_config;
    if (dab_mutex_init(&adapter->gain_diagnostics_mutex) != 0) {
        return -1;
    }
    adapter->gain_diagnostics_mutex_ready = 1;
    return 0;
}

void dab_sdrplay_adapter_destroy_base(dab_sdrplay_adapter_t *adapter) {
    if (adapter != 0 && adapter->gain_diagnostics_mutex_ready) {
        dab_mutex_destroy(&adapter->gain_diagnostics_mutex);
        adapter->gain_diagnostics_mutex_ready = 0;
    }
}

void dab_sdrplay_adapter_publish_gain_diagnostics(
    dab_sdrplay_adapter_t *adapter,
    const dab_sdrplay_gain_diagnostics_t *diagnostics) {
    if (adapter == 0 || diagnostics == 0 ||
        !adapter->gain_diagnostics_mutex_ready) {
        return;
    }
    dab_mutex_lock(&adapter->gain_diagnostics_mutex);
    adapter->gain_diagnostics = *diagnostics;
    dab_mutex_unlock(&adapter->gain_diagnostics_mutex);
}

dab_sdr_status_t dab_sdrplay_adapter_get_gain_diagnostics(
    dab_sdrplay_adapter_t *adapter,
    dab_sdrplay_gain_diagnostics_t *diagnostics) {
    if (adapter == 0 || diagnostics == 0) {
        return DAB_SDR_STATUS_INVALID_ARGUMENT;
    }
    memset(diagnostics, 0, sizeof(*diagnostics));
    if (!adapter->gain_diagnostics_mutex_ready) {
        return DAB_SDR_STATUS_ERROR;
    }
    dab_mutex_lock(&adapter->gain_diagnostics_mutex);
    *diagnostics = adapter->gain_diagnostics;
    dab_mutex_unlock(&adapter->gain_diagnostics_mutex);
    return diagnostics->valid ? DAB_SDR_STATUS_OK : DAB_SDR_STATUS_NOT_FOUND;
}

static int dab_sdrplay_use_real_adapter_requested(void) {
    const char *value = getenv("DAB_SDRPLAY_USE_REAL");
    return value != 0 && strcmp(value, "1") == 0;
}

dab_sdrplay_adapter_t *dab_sdrplay_adapter_create(const dab_stream_config_t *config,
                                                  const dab_sdrplay_config_t *device_config) {
    dab_sdrplay_api_t api;

    if (config == 0 || device_config == 0) {
        return 0;
    }

    if (dab_sdrplay_use_real_adapter_requested()) {
        dab_sdrplay_adapter_t *adapter;

        if (dab_sdrplay_api_try_load(&api) != 0) {
            return 0;
        }
        if (!dab_sdrplay_api_is_usable(&api)) {
            dab_sdrplay_api_unload(&api);
            return 0;
        }
        adapter = dab_sdrplay_real_adapter_create(config, device_config, &api);
        if (adapter == 0) {
            dab_sdrplay_api_unload(&api);
        }
        return adapter;
    }

    return dab_sdrplay_stub_adapter_create(config, device_config);
}
