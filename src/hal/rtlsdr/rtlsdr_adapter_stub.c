#include "rtlsdr_adapter.h"

#include <stdlib.h>

#include "dab/os/os_thread.h"
#include "dab/os/os_time.h"

enum {
    DAB_RTLSDR_STUB_SAMPLES_PER_CALLBACK = 1024
};

typedef struct {
    dab_rtlsdr_adapter_t base;
    dab_thread_t thread;
    dab_rtlsdr_samples_cb_t callback;
    void *user;
    uint64_t sample_index;
    int running;
    int stop_requested;
} dab_rtlsdr_stub_adapter_t;

static void dab_rtlsdr_stub_fill_iq(int16_t *i_dst,
                                    int16_t *q_dst,
                                    uint32_t sample_count,
                                    uint64_t sample_index) {
    uint32_t i;
    for (i = 0u; i < sample_count; ++i) {
        int16_t value = (int16_t)(((sample_index + i) & 0xffu) << 8u);
        i_dst[i] = value;
        q_dst[i] = (int16_t)(-value);
    }
}

static int dab_rtlsdr_stub_thread(void *arg) {
    dab_rtlsdr_stub_adapter_t *adapter = (dab_rtlsdr_stub_adapter_t *)arg;
    int16_t i_samples[DAB_RTLSDR_STUB_SAMPLES_PER_CALLBACK];
    int16_t q_samples[DAB_RTLSDR_STUB_SAMPLES_PER_CALLBACK];

    while (!adapter->stop_requested) {
        dab_rtlsdr_stub_fill_iq(i_samples,
                                q_samples,
                                DAB_RTLSDR_STUB_SAMPLES_PER_CALLBACK,
                                adapter->sample_index);
        if (adapter->callback != 0) {
            adapter->callback(i_samples,
                              q_samples,
                              DAB_RTLSDR_STUB_SAMPLES_PER_CALLBACK,
                              adapter->sample_index,
                              dab_time_monotonic_ns(),
                              adapter->user);
        }
        adapter->sample_index += DAB_RTLSDR_STUB_SAMPLES_PER_CALLBACK;
    }

    adapter->running = 0;
    return 0;
}

static dab_sdr_status_t dab_rtlsdr_stub_start(dab_rtlsdr_adapter_t *base,
                                              dab_rtlsdr_samples_cb_t callback,
                                              void *user) {
    dab_rtlsdr_stub_adapter_t *adapter = (dab_rtlsdr_stub_adapter_t *)base;

    if (adapter == 0 || callback == 0) {
        return DAB_SDR_STATUS_INVALID_ARGUMENT;
    }
    if (adapter->running) {
        return DAB_SDR_STATUS_BUSY;
    }

    adapter->callback = callback;
    adapter->user = user;
    adapter->stop_requested = 0;
    adapter->running = 1;
    if (dab_thread_create(&adapter->thread, dab_rtlsdr_stub_thread, adapter, "rtlsdr-stub") != 0) {
        adapter->running = 0;
        return DAB_SDR_STATUS_ERROR;
    }

    return DAB_SDR_STATUS_OK;
}

static dab_sdr_status_t dab_rtlsdr_stub_stop(dab_rtlsdr_adapter_t *base) {
    dab_rtlsdr_stub_adapter_t *adapter = (dab_rtlsdr_stub_adapter_t *)base;
    int rc = 0;

    if (adapter == 0) {
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

static void dab_rtlsdr_stub_destroy(dab_rtlsdr_adapter_t *base) {
    dab_rtlsdr_stub_adapter_t *adapter = (dab_rtlsdr_stub_adapter_t *)base;
    if (adapter == 0) {
        return;
    }
    if (adapter->running) {
        (void)dab_rtlsdr_stub_stop(base);
    }
    free(adapter);
}

static const dab_rtlsdr_adapter_vtbl_t DAB_RTLSDR_STUB_VTBL = {
    dab_rtlsdr_stub_start,
    dab_rtlsdr_stub_stop,
    dab_rtlsdr_stub_destroy
};

dab_rtlsdr_adapter_t *dab_rtlsdr_stub_adapter_create(const dab_stream_config_t *config,
                                                     const dab_rtlsdr_config_t *device_config,
                                                     uint32_t device_index) {
    dab_rtlsdr_stub_adapter_t *adapter;

    if (config == 0 || device_config == 0) {
        return 0;
    }

    adapter = (dab_rtlsdr_stub_adapter_t *)calloc(1u, sizeof(*adapter));
    if (adapter == 0) {
        return 0;
    }

    adapter->base.vtbl = &DAB_RTLSDR_STUB_VTBL;
    adapter->base.config = *config;
    adapter->base.device_config = *device_config;
    adapter->base.device_index = device_index;
    return &adapter->base;
}
