#include "dab/hal/sdr_device.h"

#include <stdlib.h>
#include <string.h>

typedef struct {
    dab_sdr_stream_t base;
    dab_stream_config_t config;
    int started;
} dab_sdrplay_stream_t;

typedef struct {
    dab_sdr_device_t base;
    dab_stream_config_t config;
    int configured;
} dab_sdrplay_device_t;

static dab_sdr_status_t dab_sdrplay_stream_start(struct dab_sdr_stream *stream) {
    dab_sdrplay_stream_t *impl = (dab_sdrplay_stream_t *)stream;
    if (impl == NULL) {
        return DAB_SDR_STATUS_INVALID_ARGUMENT;
    }
    impl->started = 1;
    return DAB_SDR_STATUS_NOT_IMPLEMENTED;
}

static dab_sdr_status_t dab_sdrplay_stream_stop(struct dab_sdr_stream *stream) {
    dab_sdrplay_stream_t *impl = (dab_sdrplay_stream_t *)stream;
    if (impl == NULL) {
        return DAB_SDR_STATUS_INVALID_ARGUMENT;
    }
    impl->started = 0;
    return DAB_SDR_STATUS_OK;
}

static dab_sdr_status_t dab_sdrplay_stream_read(struct dab_sdr_stream *stream, dab_sdr_block_view_t *block) {
    (void)stream;
    (void)block;
    return DAB_SDR_STATUS_NOT_IMPLEMENTED;
}

static void dab_sdrplay_stream_destroy(struct dab_sdr_stream *stream) {
    free(stream);
}

static const dab_sdr_stream_vtbl_t DAB_SDRPLAY_STREAM_VTBL = {
    dab_sdrplay_stream_start,
    dab_sdrplay_stream_stop,
    dab_sdrplay_stream_read,
    dab_sdrplay_stream_destroy
};

static int dab_sdrplay_validate_config(const dab_stream_config_t *config, const dab_sdr_capabilities_t *caps) {
    if (config == NULL || caps == NULL) {
        return 0;
    }
    if (config->sample_rate < caps->min_sample_rate || config->sample_rate > caps->max_sample_rate) {
        return 0;
    }
    if (config->center_freq_hz < caps->min_center_freq_hz || config->center_freq_hz > caps->max_center_freq_hz) {
        return 0;
    }
    if (config->gain_db < caps->min_gain_db || config->gain_db > caps->max_gain_db) {
        return 0;
    }
    return 1;
}

static dab_sdr_status_t dab_sdrplay_configure(struct dab_sdr_device *device, const dab_stream_config_t *config) {
    dab_sdrplay_device_t *impl = (dab_sdrplay_device_t *)device;
    if (impl == NULL || config == NULL) {
        return DAB_SDR_STATUS_INVALID_ARGUMENT;
    }
    if (!dab_sdrplay_validate_config(config, &impl->base.desc.caps)) {
        return DAB_SDR_STATUS_UNSUPPORTED;
    }
    impl->config = *config;
    impl->configured = 1;
    return DAB_SDR_STATUS_OK;
}

static dab_sdr_status_t dab_sdrplay_open_stream(struct dab_sdr_device *device, dab_sdr_stream_t **stream) {
    dab_sdrplay_device_t *impl = (dab_sdrplay_device_t *)device;
    dab_sdrplay_stream_t *created;

    if (impl == NULL || stream == NULL) {
        return DAB_SDR_STATUS_INVALID_ARGUMENT;
    }
    *stream = NULL;

    if (!impl->configured) {
        return DAB_SDR_STATUS_ERROR;
    }

    created = (dab_sdrplay_stream_t *)calloc(1u, sizeof(*created));
    if (created == NULL) {
        return DAB_SDR_STATUS_ERROR;
    }

    created->base.vtbl = &DAB_SDRPLAY_STREAM_VTBL;
    created->config = impl->config;
    *stream = &created->base;
    return DAB_SDR_STATUS_OK;
}

static void dab_sdrplay_destroy(struct dab_sdr_device *device) {
    free(device);
}

static const dab_sdr_device_vtbl_t DAB_SDRPLAY_DEVICE_VTBL = {
    dab_sdrplay_configure,
    dab_sdrplay_open_stream,
    dab_sdrplay_destroy
};

static const dab_sdr_device_desc_t DAB_SDRPLAY_DESCS[] = {
    {
        "sdrplay:stub:0",
        "sdrplay",
        "stub-0001",
        "SDRplay Stub Device",
        DAB_SDR_DRIVER_SDRPLAY,
        {
            2000000u,
            10000000u,
            1000000ull,
            2000000000ull,
            0,
            59
        }
    }
};

const dab_sdr_device_desc_t *dab_sdrplay_device_descs(uint32_t *count) {
    if (count != NULL) {
        *count = (uint32_t)(sizeof(DAB_SDRPLAY_DESCS) / sizeof(DAB_SDRPLAY_DESCS[0]));
    }
    return DAB_SDRPLAY_DESCS;
}

static const dab_sdr_device_desc_t *dab_sdrplay_match_desc(const dab_sdr_open_params_t *params) {
    uint32_t i;
    for (i = 0u; i < (uint32_t)(sizeof(DAB_SDRPLAY_DESCS) / sizeof(DAB_SDRPLAY_DESCS[0])); ++i) {
        const dab_sdr_device_desc_t *desc = &DAB_SDRPLAY_DESCS[i];
        if (params->device_id != NULL && strcmp(params->device_id, desc->id) == 0) {
            return desc;
        }
        if (params->serial != NULL && strcmp(params->serial, desc->serial) == 0) {
            return desc;
        }
    }
    if (params->device_id == NULL && params->serial == NULL) {
        return &DAB_SDRPLAY_DESCS[0];
    }
    return NULL;
}

dab_sdr_status_t dab_sdrplay_device_open(const dab_sdr_open_params_t *params, dab_sdr_device_t **device) {
    const dab_sdr_device_desc_t *desc;
    dab_sdrplay_device_t *created;

    if (params == NULL || device == NULL) {
        return DAB_SDR_STATUS_INVALID_ARGUMENT;
    }
    *device = NULL;

    desc = dab_sdrplay_match_desc(params);
    if (desc == NULL) {
        return DAB_SDR_STATUS_NOT_FOUND;
    }

    created = (dab_sdrplay_device_t *)calloc(1u, sizeof(*created));
    if (created == NULL) {
        return DAB_SDR_STATUS_ERROR;
    }

    created->base.vtbl = &DAB_SDRPLAY_DEVICE_VTBL;
    created->base.desc = *desc;
    *device = &created->base;
    return DAB_SDR_STATUS_OK;
}
