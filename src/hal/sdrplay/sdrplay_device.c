#include "dab/hal/sdr_device.h"

#include <stdlib.h>
#include <string.h>

#include "dab/core/block_pool.h"
#include "dab/core/pc_queue.h"
#include "sdrplay_adapter.h"
#include "sdrplay_device.h"

enum {
    DAB_SDRPLAY_STREAM_BLOCK_COUNT = DAB_CAPTURE_BLOCK_COUNT,
    DAB_SDRPLAY_STREAM_QUEUE_CAPACITY = DAB_CAPTURE_BLOCK_COUNT
};

typedef struct {
    dab_sdr_stream_t base;
    dab_sdrplay_adapter_t *adapter;
    dab_stream_config_t config;
    dab_iq_block_pool_t pool;
    dab_pc_queue_t filled_blocks;
    dab_iq_block_storage_t storage;
    dab_iq_block_t *write_block;
    dab_iq_block_t *current_block;
    uint32_t dropped_blocks;
    int pool_ready;
    int filled_queue_ready;
    int started;
} dab_sdrplay_stream_t;

typedef struct {
    dab_sdr_device_t base;
    dab_stream_config_t config;
    dab_sdrplay_config_t sdrplay_config;
    int configured;
} dab_sdrplay_device_t;

static void dab_sdrplay_stream_on_samples(const int16_t *i_samples,
                                          const int16_t *q_samples,
                                          uint32_t sample_count,
                                          uint64_t sample_index,
                                          uint64_t timestamp_ns,
                                          void *user) {
    dab_sdrplay_stream_t *stream = (dab_sdrplay_stream_t *)user;
    uint32_t input_offset = 0u;

    if (stream == NULL || i_samples == NULL || q_samples == NULL || sample_count == 0u) {
        return;
    }

    while (input_offset < sample_count) {
        dab_iq_block_t *block = stream->write_block;
        uint32_t copy_count;
        uint32_t i;

        if (block != NULL &&
            block->sequence + block->valid_samples != sample_index + input_offset) {
            stream->write_block = NULL;
            if (dab_pc_queue_try_push(&stream->filled_blocks, block) != 0) {
                stream->dropped_blocks++;
                (void)dab_iq_block_release(&stream->pool, block);
            }
            block = NULL;
        }
        if (block == NULL) {
            if (dab_pc_queue_try_pop(&stream->pool.free_blocks, (void **)&block) != 0) {
                stream->dropped_blocks++;
                return;
            }
            block->sequence = sample_index + input_offset;
            block->timestamp_ns = timestamp_ns;
            block->valid_samples = 0u;
            block->flags = 0u;
            stream->write_block = block;
        }

        copy_count = sample_count - input_offset;
        if (copy_count > DAB_IQ_BLOCK_SAMPLES - block->valid_samples) {
            copy_count = DAB_IQ_BLOCK_SAMPLES - block->valid_samples;
        }
        for (i = 0u; i < copy_count; ++i) {
            const uint32_t dst = block->valid_samples + i;
            block->iq[2u * dst] = i_samples[input_offset + i];
            block->iq[(2u * dst) + 1u] = q_samples[input_offset + i];
        }
        block->valid_samples += copy_count;
        input_offset += copy_count;

        if (block->valid_samples == DAB_IQ_BLOCK_SAMPLES) {
            stream->write_block = NULL;
            if (dab_pc_queue_try_push(&stream->filled_blocks, block) != 0) {
                stream->dropped_blocks++;
                (void)dab_iq_block_release(&stream->pool, block);
            }
        }
    }
}

static void dab_sdrplay_stream_teardown(dab_sdrplay_stream_t *stream) {
    if (stream->adapter != NULL) {
        stream->adapter->vtbl->destroy(stream->adapter);
        stream->adapter = NULL;
    }
    if (stream->write_block != NULL) {
        (void)dab_iq_block_release(&stream->pool, stream->write_block);
        stream->write_block = NULL;
    }
    if (stream->filled_queue_ready) {
        dab_pc_queue_destroy(&stream->filled_blocks);
        stream->filled_queue_ready = 0;
    }
    if (stream->pool_ready) {
        dab_iq_block_pool_destroy(&stream->pool);
        stream->pool_ready = 0;
    }
    dab_iq_block_storage_destroy(&stream->storage);
    stream->current_block = NULL;
}

static dab_sdr_status_t dab_sdrplay_stream_start(struct dab_sdr_stream *stream) {
    dab_sdrplay_stream_t *impl = (dab_sdrplay_stream_t *)stream;
    if (impl == NULL) {
        return DAB_SDR_STATUS_INVALID_ARGUMENT;
    }
    if (impl->started) {
        return DAB_SDR_STATUS_BUSY;
    }
    if (impl->adapter->vtbl->start(impl->adapter, dab_sdrplay_stream_on_samples, impl) != DAB_SDR_STATUS_OK) {
        return DAB_SDR_STATUS_ERROR;
    }
    impl->started = 1;
    return DAB_SDR_STATUS_OK;
}

static dab_sdr_status_t dab_sdrplay_stream_stop(struct dab_sdr_stream *stream) {
    dab_sdrplay_stream_t *impl = (dab_sdrplay_stream_t *)stream;
    dab_sdr_status_t status;

    if (impl == NULL) {
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

static dab_sdr_status_t dab_sdrplay_stream_read_internal(struct dab_sdr_stream *stream,
                                                         dab_sdr_block_view_t *block,
                                                         uint32_t timeout_ms) {
    dab_sdrplay_stream_t *impl = (dab_sdrplay_stream_t *)stream;
    dab_iq_block_t *next_block = NULL;
    int queue_result;

    if (impl == NULL || block == NULL) {
        return DAB_SDR_STATUS_INVALID_ARGUMENT;
    }
    if (!impl->started) {
        return DAB_SDR_STATUS_ERROR;
    }

    if (impl->current_block != NULL) {
        (void)dab_iq_block_release(&impl->pool, impl->current_block);
        impl->current_block = NULL;
    }

    queue_result = timeout_ms == 0u
        ? dab_pc_queue_pop(&impl->filled_blocks, (void **)&next_block)
        : dab_pc_queue_pop_timeout(&impl->filled_blocks,
                                   (void **)&next_block,
                                   timeout_ms);
    if (queue_result == DAB_PC_QUEUE_TIMEOUT) {
        return DAB_SDR_STATUS_TIMEOUT;
    }
    if (queue_result != DAB_PC_QUEUE_OK) {
        return DAB_SDR_STATUS_ERROR;
    }

    impl->current_block = next_block;
    block->iq_interleaved = next_block->iq;
    block->sample_count = next_block->valid_samples;
    block->sample_index = next_block->sequence;
    block->timestamp_ns = next_block->timestamp_ns;
    return DAB_SDR_STATUS_OK;
}

static void dab_sdrplay_stream_destroy(struct dab_sdr_stream *stream) {
    dab_sdrplay_stream_t *impl = (dab_sdrplay_stream_t *)stream;
    if (impl == NULL) {
        return;
    }
    if (impl->started) {
        (void)dab_sdrplay_stream_stop(stream);
    }
    if (impl->current_block != NULL) {
        (void)dab_iq_block_release(&impl->pool, impl->current_block);
        impl->current_block = NULL;
    }
    dab_sdrplay_stream_teardown(impl);
    free(impl);
}

static dab_sdr_status_t dab_sdrplay_stream_diagnostics(
    struct dab_sdr_stream *stream,
    dab_sdr_stream_diagnostics_t *diagnostics) {
    dab_sdrplay_stream_t *impl = (dab_sdrplay_stream_t *)stream;
    if (impl == NULL || diagnostics == NULL) {
        return DAB_SDR_STATUS_INVALID_ARGUMENT;
    }
    memset(diagnostics, 0, sizeof(*diagnostics));
    if (impl->filled_queue_ready) {
        dab_pc_queue_usage(&impl->filled_blocks,
                           &diagnostics->queue_depth,
                           &diagnostics->queue_high_watermark,
                           &diagnostics->queue_capacity);
    }
    diagnostics->dropped_blocks = impl->dropped_blocks;
    return DAB_SDR_STATUS_OK;
}

static dab_sdr_status_t dab_sdrplay_stream_read(struct dab_sdr_stream *stream,
                                                 dab_sdr_block_view_t *block) {
    return dab_sdrplay_stream_read_internal(stream, block, 0u);
}

static dab_sdr_status_t dab_sdrplay_stream_read_timeout(struct dab_sdr_stream *stream,
                                                         dab_sdr_block_view_t *block,
                                                         uint32_t timeout_ms) {
    return dab_sdrplay_stream_read_internal(stream, block, timeout_ms);
}

static const dab_sdr_stream_vtbl_t DAB_SDRPLAY_STREAM_VTBL = {
    dab_sdrplay_stream_start,
    dab_sdrplay_stream_stop,
    dab_sdrplay_stream_read,
    dab_sdrplay_stream_read_timeout,
    dab_sdrplay_stream_diagnostics,
    dab_sdrplay_stream_destroy
};

void dab_sdrplay_default_config(dab_sdrplay_config_t *config) {
    if (config == NULL) {
        return;
    }
    config->gain_mode = DAB_SDRPLAY_GAIN_MODE_AUTO;
    config->if_mode = DAB_SDRPLAY_IF_MODE_ZERO_IF;
    config->antenna = DAB_SDRPLAY_ANTENNA_AUTO;
    config->lna_state = 0u;
    config->if_gain_db = 0;
    config->bias_tee_enabled = 0;
    config->ppm_correction = 0;
}

int dab_sdrplay_validate_device_config(const dab_sdrplay_config_t *config) {
    if (config == NULL) {
        return 0;
    }
    if (config->gain_mode != DAB_SDRPLAY_GAIN_MODE_AUTO &&
        config->gain_mode != DAB_SDRPLAY_GAIN_MODE_MANUAL) {
        return 0;
    }
    if (config->if_mode != DAB_SDRPLAY_IF_MODE_ZERO_IF &&
        config->if_mode != DAB_SDRPLAY_IF_MODE_LOW_IF) {
        return 0;
    }
    if (config->antenna != DAB_SDRPLAY_ANTENNA_AUTO &&
        config->antenna != DAB_SDRPLAY_ANTENNA_A &&
        config->antenna != DAB_SDRPLAY_ANTENNA_B) {
        return 0;
    }
    if (config->lna_state > 15u) {
        return 0;
    }
    if (config->if_gain_db < 0 || config->if_gain_db > 59) {
        return 0;
    }
    if (config->bias_tee_enabled != 0 && config->bias_tee_enabled != 1) {
        return 0;
    }
    if (config->ppm_correction < -200 || config->ppm_correction > 200) {
        return 0;
    }
    return 1;
}

dab_sdr_status_t dab_sdrplay_configure_device(dab_sdr_device_t *device, const dab_sdrplay_config_t *config) {
    dab_sdrplay_device_t *impl = (dab_sdrplay_device_t *)device;

    if (impl == NULL || config == NULL) {
        return DAB_SDR_STATUS_INVALID_ARGUMENT;
    }
    if (impl->base.desc.kind != DAB_SDR_DRIVER_SDRPLAY) {
        return DAB_SDR_STATUS_UNSUPPORTED;
    }
    if (!dab_sdrplay_validate_device_config(config)) {
        return DAB_SDR_STATUS_UNSUPPORTED;
    }

    impl->sdrplay_config = *config;
    return DAB_SDR_STATUS_OK;
}

uint32_t dab_sdrplay_stream_dropped_blocks(const dab_sdr_stream_t *stream) {
    const dab_sdrplay_stream_t *impl = (const dab_sdrplay_stream_t *)stream;

    if (impl == NULL) {
        return 0u;
    }
    return impl->dropped_blocks;
}

dab_sdr_status_t dab_sdrplay_stream_get_gain_diagnostics(
    dab_sdr_stream_t *stream,
    dab_sdrplay_gain_diagnostics_t *diagnostics) {
    dab_sdrplay_stream_t *impl = (dab_sdrplay_stream_t *)stream;

    if (impl == NULL || diagnostics == NULL) {
        return DAB_SDR_STATUS_INVALID_ARGUMENT;
    }
    if (impl->adapter == NULL) {
        memset(diagnostics, 0, sizeof(*diagnostics));
        return DAB_SDR_STATUS_ERROR;
    }
    return dab_sdrplay_adapter_get_gain_diagnostics(impl->adapter, diagnostics);
}

static int dab_sdrplay_validate_stream_config(const dab_stream_config_t *config, const dab_sdr_capabilities_t *caps) {
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
    if (!dab_sdrplay_validate_stream_config(config, &impl->base.desc.caps)) {
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

    if (dab_iq_block_storage_init(&created->storage,
                                  DAB_SDRPLAY_STREAM_BLOCK_COUNT,
                                  DAB_SDRPLAY_STREAM_QUEUE_CAPACITY) != 0) {
        dab_sdrplay_stream_destroy(&created->base);
        return DAB_SDR_STATUS_ERROR;
    }
    if (dab_iq_block_pool_init(&created->pool,
                               created->storage.blocks,
                               created->storage.free_storage,
                               DAB_SDRPLAY_STREAM_BLOCK_COUNT) != 0) {
        dab_sdrplay_stream_destroy(&created->base);
        return DAB_SDR_STATUS_ERROR;
    }
    created->pool_ready = 1;
    if (dab_pc_queue_init(&created->filled_blocks,
                          created->storage.filled_storage,
                          DAB_SDRPLAY_STREAM_QUEUE_CAPACITY) != 0) {
        dab_sdrplay_stream_destroy(&created->base);
        return DAB_SDR_STATUS_ERROR;
    }
    created->filled_queue_ready = 1;
    created->adapter = dab_sdrplay_adapter_create(&impl->config, &impl->sdrplay_config);
    if (created->adapter == NULL) {
        dab_sdrplay_stream_destroy(&created->base);
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
    dab_sdrplay_default_config(&created->sdrplay_config);
    *device = &created->base;
    return DAB_SDR_STATUS_OK;
}
