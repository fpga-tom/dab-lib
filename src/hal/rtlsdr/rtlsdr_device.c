#include "dab/hal/rtlsdr_device.h"

#include <stdlib.h>
#include <string.h>

#include "dab/core/block_pool.h"
#include "dab/core/pc_queue.h"
#include "rtlsdr_adapter.h"

enum {
    DAB_RTLSDR_STREAM_BLOCK_COUNT = 2048,
    DAB_RTLSDR_STREAM_QUEUE_CAPACITY = 2048
};

typedef struct {
    dab_sdr_stream_t base;
    dab_rtlsdr_adapter_t *adapter;
    dab_stream_config_t config;
    dab_iq_block_pool_t pool;
    dab_pc_queue_t filled_blocks;
    dab_iq_block_t *blocks;
    void **free_storage;
    void **filled_storage;
    dab_iq_block_t *current_block;
    uint32_t dropped_blocks;
    int pool_ready;
    int filled_queue_ready;
    int started;
} dab_rtlsdr_stream_t;

typedef struct {
    dab_sdr_device_t base;
    dab_stream_config_t config;
    dab_rtlsdr_config_t rtlsdr_config;
    uint32_t device_index;
    int configured;
} dab_rtlsdr_device_t;

static void dab_rtlsdr_stream_on_samples(const int16_t *i_samples,
                                         const int16_t *q_samples,
                                         uint32_t sample_count,
                                         uint64_t sample_index,
                                         uint64_t timestamp_ns,
                                         void *user) {
    dab_rtlsdr_stream_t *stream = (dab_rtlsdr_stream_t *)user;
    dab_iq_block_t *block = 0;
    uint32_t copy_count;
    uint32_t i;

    if (stream == 0 || i_samples == 0 || q_samples == 0 || sample_count == 0u) {
        return;
    }

    if (dab_pc_queue_try_pop(&stream->pool.free_blocks, (void **)&block) != 0) {
        stream->dropped_blocks++;
        return;
    }

    copy_count = sample_count > DAB_IQ_BLOCK_SAMPLES ? DAB_IQ_BLOCK_SAMPLES : sample_count;
    for (i = 0u; i < copy_count; ++i) {
        block->iq[2u * i] = i_samples[i];
        block->iq[(2u * i) + 1u] = q_samples[i];
    }
    block->sequence = sample_index;
    block->timestamp_ns = timestamp_ns;
    block->valid_samples = copy_count;
    block->flags = 0u;

    if (dab_pc_queue_try_push(&stream->filled_blocks, block) != 0) {
        stream->dropped_blocks++;
        (void)dab_iq_block_release(&stream->pool, block);
    }
}

static void dab_rtlsdr_stream_teardown(dab_rtlsdr_stream_t *stream) {
    if (stream->adapter != 0) {
        stream->adapter->vtbl->destroy(stream->adapter);
        stream->adapter = 0;
    }
    if (stream->filled_queue_ready) {
        dab_pc_queue_destroy(&stream->filled_blocks);
        stream->filled_queue_ready = 0;
    }
    free(stream->filled_storage);
    stream->filled_storage = 0;
    if (stream->pool_ready) {
        dab_iq_block_pool_destroy(&stream->pool);
        stream->pool_ready = 0;
    }
    free(stream->free_storage);
    stream->free_storage = 0;
    free(stream->blocks);
    stream->blocks = 0;
    stream->current_block = 0;
}

static dab_sdr_status_t dab_rtlsdr_stream_start(struct dab_sdr_stream *stream) {
    dab_rtlsdr_stream_t *impl = (dab_rtlsdr_stream_t *)stream;
    if (impl == 0) {
        return DAB_SDR_STATUS_INVALID_ARGUMENT;
    }
    if (impl->started) {
        return DAB_SDR_STATUS_BUSY;
    }
    if (impl->adapter->vtbl->start(impl->adapter, dab_rtlsdr_stream_on_samples, impl) != DAB_SDR_STATUS_OK) {
        return DAB_SDR_STATUS_ERROR;
    }
    impl->started = 1;
    return DAB_SDR_STATUS_OK;
}

static dab_sdr_status_t dab_rtlsdr_stream_stop(struct dab_sdr_stream *stream) {
    dab_rtlsdr_stream_t *impl = (dab_rtlsdr_stream_t *)stream;
    dab_sdr_status_t status;

    if (impl == 0) {
        return DAB_SDR_STATUS_INVALID_ARGUMENT;
    }
    if (!impl->started) {
        return DAB_SDR_STATUS_OK;
    }

    status = impl->adapter->vtbl->stop(impl->adapter);
    dab_pc_queue_stop(&impl->filled_blocks);
    impl->started = 0;
    return status;
}

static dab_sdr_status_t dab_rtlsdr_stream_read(struct dab_sdr_stream *stream, dab_sdr_block_view_t *block) {
    dab_rtlsdr_stream_t *impl = (dab_rtlsdr_stream_t *)stream;
    dab_iq_block_t *next_block = 0;

    if (impl == 0 || block == 0) {
        return DAB_SDR_STATUS_INVALID_ARGUMENT;
    }
    if (!impl->started) {
        return DAB_SDR_STATUS_ERROR;
    }
    if (impl->current_block != 0) {
        (void)dab_iq_block_release(&impl->pool, impl->current_block);
        impl->current_block = 0;
    }
    if (dab_pc_queue_pop(&impl->filled_blocks, (void **)&next_block) != 0) {
        return DAB_SDR_STATUS_ERROR;
    }

    impl->current_block = next_block;
    block->iq_interleaved = next_block->iq;
    block->sample_count = next_block->valid_samples;
    block->sample_index = next_block->sequence;
    block->timestamp_ns = next_block->timestamp_ns;
    return DAB_SDR_STATUS_OK;
}

static void dab_rtlsdr_stream_destroy(struct dab_sdr_stream *stream) {
    dab_rtlsdr_stream_t *impl = (dab_rtlsdr_stream_t *)stream;
    if (impl == 0) {
        return;
    }
    if (impl->started) {
        (void)dab_rtlsdr_stream_stop(stream);
    }
    if (impl->current_block != 0) {
        (void)dab_iq_block_release(&impl->pool, impl->current_block);
        impl->current_block = 0;
    }
    dab_rtlsdr_stream_teardown(impl);
    free(impl);
}

static const dab_sdr_stream_vtbl_t DAB_RTLSDR_STREAM_VTBL = {
    dab_rtlsdr_stream_start,
    dab_rtlsdr_stream_stop,
    dab_rtlsdr_stream_read,
    dab_rtlsdr_stream_destroy
};

void dab_rtlsdr_default_config(dab_rtlsdr_config_t *config) {
    if (config == 0) {
        return;
    }
    config->gain_mode = DAB_RTLSDR_GAIN_MODE_AUTO;
    config->ppm_correction = 0;
    config->bias_tee_enabled = 0;
    config->android_usb_fd = -1;
}

static int dab_rtlsdr_validate_device_config(const dab_rtlsdr_config_t *config) {
    if (config == 0) {
        return 0;
    }
    if (config->gain_mode != DAB_RTLSDR_GAIN_MODE_AUTO &&
        config->gain_mode != DAB_RTLSDR_GAIN_MODE_MANUAL) {
        return 0;
    }
    if (config->ppm_correction < -200 || config->ppm_correction > 200) {
        return 0;
    }
    if (config->bias_tee_enabled != 0 && config->bias_tee_enabled != 1) {
        return 0;
    }
    return 1;
}

dab_sdr_status_t dab_rtlsdr_configure_device(dab_sdr_device_t *device, const dab_rtlsdr_config_t *config) {
    dab_rtlsdr_device_t *impl = (dab_rtlsdr_device_t *)device;

    if (impl == 0 || config == 0) {
        return DAB_SDR_STATUS_INVALID_ARGUMENT;
    }
    if (impl->base.desc.kind != DAB_SDR_DRIVER_RTLSDR) {
        return DAB_SDR_STATUS_UNSUPPORTED;
    }
    if (!dab_rtlsdr_validate_device_config(config)) {
        return DAB_SDR_STATUS_UNSUPPORTED;
    }

    impl->rtlsdr_config = *config;
    return DAB_SDR_STATUS_OK;
}

uint32_t dab_rtlsdr_stream_dropped_blocks(const dab_sdr_stream_t *stream) {
    const dab_rtlsdr_stream_t *impl = (const dab_rtlsdr_stream_t *)stream;
    return impl == 0 ? 0u : impl->dropped_blocks;
}

static int dab_rtlsdr_validate_stream_config(const dab_stream_config_t *config, const dab_sdr_capabilities_t *caps) {
    if (config == 0 || caps == 0) {
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

static dab_sdr_status_t dab_rtlsdr_configure(struct dab_sdr_device *device, const dab_stream_config_t *config) {
    dab_rtlsdr_device_t *impl = (dab_rtlsdr_device_t *)device;
    if (impl == 0 || config == 0) {
        return DAB_SDR_STATUS_INVALID_ARGUMENT;
    }
    if (!dab_rtlsdr_validate_stream_config(config, &impl->base.desc.caps)) {
        return DAB_SDR_STATUS_UNSUPPORTED;
    }
    impl->config = *config;
    impl->configured = 1;
    return DAB_SDR_STATUS_OK;
}

static dab_sdr_status_t dab_rtlsdr_open_stream(struct dab_sdr_device *device, dab_sdr_stream_t **stream) {
    dab_rtlsdr_device_t *impl = (dab_rtlsdr_device_t *)device;
    dab_rtlsdr_stream_t *created;

    if (impl == 0 || stream == 0) {
        return DAB_SDR_STATUS_INVALID_ARGUMENT;
    }
    *stream = 0;
    if (!impl->configured) {
        return DAB_SDR_STATUS_ERROR;
    }

    created = (dab_rtlsdr_stream_t *)calloc(1u, sizeof(*created));
    if (created == 0) {
        return DAB_SDR_STATUS_ERROR;
    }
    created->blocks = (dab_iq_block_t *)calloc(DAB_RTLSDR_STREAM_BLOCK_COUNT, sizeof(*created->blocks));
    created->free_storage = (void **)calloc(DAB_RTLSDR_STREAM_BLOCK_COUNT, sizeof(*created->free_storage));
    created->filled_storage = (void **)calloc(DAB_RTLSDR_STREAM_QUEUE_CAPACITY, sizeof(*created->filled_storage));
    if (created->blocks == 0 || created->free_storage == 0 || created->filled_storage == 0) {
        dab_rtlsdr_stream_destroy(&created->base);
        return DAB_SDR_STATUS_ERROR;
    }
    if (dab_iq_block_pool_init(&created->pool, created->blocks, created->free_storage, DAB_RTLSDR_STREAM_BLOCK_COUNT) != 0) {
        dab_rtlsdr_stream_destroy(&created->base);
        return DAB_SDR_STATUS_ERROR;
    }
    created->pool_ready = 1;
    if (dab_pc_queue_init(&created->filled_blocks, created->filled_storage, DAB_RTLSDR_STREAM_QUEUE_CAPACITY) != 0) {
        dab_rtlsdr_stream_destroy(&created->base);
        return DAB_SDR_STATUS_ERROR;
    }
    created->filled_queue_ready = 1;
    created->adapter = dab_rtlsdr_adapter_create(&impl->config, &impl->rtlsdr_config, impl->device_index);
    if (created->adapter == 0) {
        dab_rtlsdr_stream_destroy(&created->base);
        return DAB_SDR_STATUS_ERROR;
    }

    created->base.vtbl = &DAB_RTLSDR_STREAM_VTBL;
    created->config = impl->config;
    *stream = &created->base;
    return DAB_SDR_STATUS_OK;
}

static void dab_rtlsdr_destroy(struct dab_sdr_device *device) {
    free(device);
}

static const dab_sdr_device_vtbl_t DAB_RTLSDR_DEVICE_VTBL = {
    dab_rtlsdr_configure,
    dab_rtlsdr_open_stream,
    dab_rtlsdr_destroy
};

static const dab_sdr_device_desc_t DAB_RTLSDR_DESCS[] = {
    {
        "rtlsdr:stub:0",
        "rtlsdr",
        "stub-0001",
        "RTL-SDR Stub Device",
        DAB_SDR_DRIVER_RTLSDR,
        {
            900000u,
            3200000u,
            24000000ull,
            1766000000ull,
            0,
            49
        }
    }
};

const dab_sdr_device_desc_t *dab_rtlsdr_device_descs(uint32_t *count) {
    if (count != 0) {
        *count = (uint32_t)(sizeof(DAB_RTLSDR_DESCS) / sizeof(DAB_RTLSDR_DESCS[0]));
    }
    return DAB_RTLSDR_DESCS;
}

static const dab_sdr_device_desc_t *dab_rtlsdr_match_desc(const dab_sdr_open_params_t *params) {
    uint32_t i;
    for (i = 0u; i < (uint32_t)(sizeof(DAB_RTLSDR_DESCS) / sizeof(DAB_RTLSDR_DESCS[0])); ++i) {
        const dab_sdr_device_desc_t *desc = &DAB_RTLSDR_DESCS[i];
        if (params->device_id != 0 && strcmp(params->device_id, desc->id) == 0) {
            return desc;
        }
        if (params->serial != 0 && strcmp(params->serial, desc->serial) == 0) {
            return desc;
        }
    }
    if (params->device_id == 0 && params->serial == 0) {
        return &DAB_RTLSDR_DESCS[0];
    }
    return 0;
}

dab_sdr_status_t dab_rtlsdr_device_open(const dab_sdr_open_params_t *params, dab_sdr_device_t **device) {
    const dab_sdr_device_desc_t *desc;
    dab_rtlsdr_device_t *created;

    if (params == 0 || device == 0) {
        return DAB_SDR_STATUS_INVALID_ARGUMENT;
    }
    *device = 0;

    desc = dab_rtlsdr_match_desc(params);
    if (desc == 0) {
        return DAB_SDR_STATUS_NOT_FOUND;
    }

    created = (dab_rtlsdr_device_t *)calloc(1u, sizeof(*created));
    if (created == 0) {
        return DAB_SDR_STATUS_ERROR;
    }

    created->base.vtbl = &DAB_RTLSDR_DEVICE_VTBL;
    created->base.desc = *desc;
    created->device_index = 0u;
    dab_rtlsdr_default_config(&created->rtlsdr_config);
    *device = &created->base;
    return DAB_SDR_STATUS_OK;
}
