#include "rtlsdr_adapter.h"

#include <stdlib.h>
#include <string.h>

static int dab_rtlsdr_use_real_adapter_requested(void) {
    const char *value = getenv("DAB_RTLSDR_USE_REAL");
    return value != 0 && strcmp(value, "1") == 0;
}

dab_rtlsdr_adapter_t *dab_rtlsdr_adapter_create(const dab_stream_config_t *config,
                                                const dab_rtlsdr_config_t *device_config,
                                                uint32_t device_index) {
    dab_rtlsdr_api_t api;

    if (config == 0 || device_config == 0) {
        return 0;
    }

    if (dab_rtlsdr_use_real_adapter_requested() &&
        dab_rtlsdr_api_try_load(&api) == 0 &&
        dab_rtlsdr_api_is_usable(&api)) {
        dab_rtlsdr_adapter_t *adapter = dab_rtlsdr_real_adapter_create(config, device_config, device_index, &api);
        if (adapter != 0) {
            return adapter;
        }
        dab_rtlsdr_api_unload(&api);
    }

    return dab_rtlsdr_stub_adapter_create(config, device_config, device_index);
}
