#include "sdrplay_adapter.h"

#include <stdlib.h>
#include <string.h>

#include "dab/os/os_thread.h"
#include "dab/os/os_time.h"

enum {
    DAB_SDRPLAY_STUB_SAMPLES_PER_CALLBACK = 1024
};

typedef struct {
    dab_sdrplay_adapter_t base;
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

static int dab_sdrplay_stub_thread(void *arg) {
    dab_sdrplay_stub_adapter_t *adapter = (dab_sdrplay_stub_adapter_t *)arg;
    int16_t iq[2u * DAB_SDRPLAY_STUB_SAMPLES_PER_CALLBACK];

    while (!adapter->stop_requested) {
        dab_sdrplay_stub_fill_iq(iq, DAB_SDRPLAY_STUB_SAMPLES_PER_CALLBACK, adapter->sample_index);
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
    free(adapter);
}

static const dab_sdrplay_adapter_vtbl_t DAB_SDRPLAY_STUB_VTBL = {
    dab_sdrplay_stub_start,
    dab_sdrplay_stub_stop,
    dab_sdrplay_stub_destroy
};

dab_sdrplay_adapter_t *dab_sdrplay_adapter_create(const dab_stream_config_t *config) {
    dab_sdrplay_stub_adapter_t *adapter;

    if (config == NULL) {
        return NULL;
    }

    adapter = (dab_sdrplay_stub_adapter_t *)calloc(1u, sizeof(*adapter));
    if (adapter == NULL) {
        return NULL;
    }

    adapter->base.vtbl = &DAB_SDRPLAY_STUB_VTBL;
    adapter->base.config = *config;
    return &adapter->base;
}
