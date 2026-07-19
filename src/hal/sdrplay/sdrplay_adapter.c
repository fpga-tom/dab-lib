#include "sdrplay_adapter.h"

#include <stdlib.h>
#include <string.h>

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
