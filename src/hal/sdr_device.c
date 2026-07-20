#include "dab/hal/sdr_device.h"

#include <stddef.h>
#include <string.h>

const dab_sdr_device_desc_t *dab_sdrplay_device_descs(uint32_t *count);
dab_sdr_status_t dab_sdrplay_device_open(const dab_sdr_open_params_t *params, dab_sdr_device_t **device);
const dab_sdr_device_desc_t *dab_rtlsdr_device_descs(uint32_t *count);
dab_sdr_status_t dab_rtlsdr_device_open(const dab_sdr_open_params_t *params, dab_sdr_device_t **device);

typedef struct {
    const char *driver;
    const dab_sdr_device_desc_t *(*descs)(uint32_t *count);
    dab_sdr_status_t (*open)(const dab_sdr_open_params_t *params, dab_sdr_device_t **device);
} dab_sdr_driver_registry_t;

static const dab_sdr_driver_registry_t DAB_DRIVER_REGISTRY[] = {
    {"sdrplay", dab_sdrplay_device_descs, dab_sdrplay_device_open},
    {"rtlsdr", dab_rtlsdr_device_descs, dab_rtlsdr_device_open}
};

static const dab_sdr_driver_registry_t *dab_sdr_find_driver(const char *driver) {
    uint32_t i;
    if (driver == NULL) {
        return NULL;
    }
    for (i = 0u; i < (uint32_t)(sizeof(DAB_DRIVER_REGISTRY) / sizeof(DAB_DRIVER_REGISTRY[0])); ++i) {
        if (strcmp(DAB_DRIVER_REGISTRY[i].driver, driver) == 0) {
            return &DAB_DRIVER_REGISTRY[i];
        }
    }
    return NULL;
}

uint32_t dab_sdr_device_count(void) {
    uint32_t count = 0u;
    uint32_t i;
    for (i = 0u; i < (uint32_t)(sizeof(DAB_DRIVER_REGISTRY) / sizeof(DAB_DRIVER_REGISTRY[0])); ++i) {
        uint32_t driver_count = 0u;
        (void)DAB_DRIVER_REGISTRY[i].descs(&driver_count);
        count += driver_count;
    }
    return count;
}

const dab_sdr_device_desc_t *dab_sdr_device_desc(uint32_t index) {
    uint32_t i;
    uint32_t base = 0u;
    for (i = 0u; i < (uint32_t)(sizeof(DAB_DRIVER_REGISTRY) / sizeof(DAB_DRIVER_REGISTRY[0])); ++i) {
        uint32_t driver_count = 0u;
        const dab_sdr_device_desc_t *descs = DAB_DRIVER_REGISTRY[i].descs(&driver_count);
        if (index < (base + driver_count)) {
            return &descs[index - base];
        }
        base += driver_count;
    }
    return NULL;
}

dab_sdr_status_t dab_sdr_device_open(const dab_sdr_open_params_t *params, dab_sdr_device_t **device) {
    const dab_sdr_driver_registry_t *driver;

    if (params == NULL || device == NULL) {
        return DAB_SDR_STATUS_INVALID_ARGUMENT;
    }

    *device = NULL;
    driver = dab_sdr_find_driver(params->driver);
    if (driver == NULL) {
        return DAB_SDR_STATUS_NOT_FOUND;
    }

    return driver->open(params, device);
}

void dab_sdr_device_close(dab_sdr_device_t *device) {
    if (device != NULL && device->vtbl != NULL && device->vtbl->destroy != NULL) {
        device->vtbl->destroy(device);
    }
}

void dab_sdr_stream_close(dab_sdr_stream_t *stream) {
    if (stream != NULL && stream->vtbl != NULL && stream->vtbl->destroy != NULL) {
        stream->vtbl->destroy(stream);
    }
}

dab_sdr_status_t dab_sdr_stream_get_diagnostics(dab_sdr_stream_t *stream,
                                                 dab_sdr_stream_diagnostics_t *diagnostics) {
    if (stream == NULL || stream->vtbl == NULL || diagnostics == NULL) {
        return DAB_SDR_STATUS_INVALID_ARGUMENT;
    }
    memset(diagnostics, 0, sizeof(*diagnostics));
    if (stream->vtbl->diagnostics == NULL) {
        return DAB_SDR_STATUS_NOT_IMPLEMENTED;
    }
    return stream->vtbl->diagnostics(stream, diagnostics);
}
