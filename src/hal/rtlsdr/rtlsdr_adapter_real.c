#include "rtlsdr_adapter.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dab/os/os_thread.h"
#include "dab/os/os_time.h"

enum {
    DAB_RTLSDR_ASYNC_BUFFER_COUNT = 16,
    DAB_RTLSDR_ASYNC_BUFFER_BYTES = 32768
};

typedef struct {
    dab_rtlsdr_adapter_t base;
    dab_rtlsdr_api_t api;
    dab_rtlsdr_dev_t *dev;
    dab_thread_t thread;
    dab_rtlsdr_samples_cb_t callback;
    void *user;
    uint64_t sample_index;
    int opened;
    int running;
    int read_rc;
} dab_rtlsdr_real_adapter_t;

static dab_sdr_status_t dab_rtlsdr_real_stop(dab_rtlsdr_adapter_t *base);

static int dab_rtlsdr_debug_enabled(void) {
    const char *value = getenv("DAB_RTLSDR_DEBUG");
    return value != 0 && value[0] == '1';
}

static void dab_rtlsdr_debug(const char *msg) {
    if (dab_rtlsdr_debug_enabled()) {
        fprintf(stderr, "rtlsdr-real: %s\n", msg);
        fflush(stderr);
    }
}

static void dab_rtlsdr_debug_rc(const char *name, int rc) {
    if (dab_rtlsdr_debug_enabled()) {
        fprintf(stderr, "rtlsdr-real: %s rc=%d\n", name, rc);
        fflush(stderr);
    }
}

static void dab_rtlsdr_warn_rc(const char *name, int rc) {
    fprintf(stderr, "rtlsdr-real: warning: %s failed rc=%d; continuing\n", name, rc);
    fflush(stderr);
}

static void dab_rtlsdr_error_rc(const char *name, int rc) {
    fprintf(stderr, "rtlsdr-real: error: %s failed rc=%d\n", name, rc);
    fflush(stderr);
}

static int dab_rtlsdr_nearest_gain(dab_rtlsdr_real_adapter_t *adapter, int requested_gain_tenths) {
    int gains[128];
    int count;
    int best_gain;
    int best_delta;
    int i;

    if (adapter->api.get_tuner_gains == 0) {
        return requested_gain_tenths;
    }

    count = adapter->api.get_tuner_gains(adapter->dev, gains);
    if (count <= 0) {
        return requested_gain_tenths;
    }
    if (count > (int)(sizeof(gains) / sizeof(gains[0]))) {
        count = (int)(sizeof(gains) / sizeof(gains[0]));
    }

    best_gain = gains[0];
    best_delta = abs(gains[0] - requested_gain_tenths);
    for (i = 1; i < count; ++i) {
        const int delta = abs(gains[i] - requested_gain_tenths);
        if (delta < best_delta) {
            best_delta = delta;
            best_gain = gains[i];
        }
    }
    return best_gain;
}

static void dab_rtlsdr_real_async_cb(unsigned char *buf, uint32_t len, void *ctx) {
    dab_rtlsdr_real_adapter_t *adapter = (dab_rtlsdr_real_adapter_t *)ctx;
    int16_t i_samples[DAB_RTLSDR_ASYNC_BUFFER_BYTES / 2u];
    int16_t q_samples[DAB_RTLSDR_ASYNC_BUFFER_BYTES / 2u];
    uint32_t sample_count;
    uint32_t i;

    if (adapter == 0 || adapter->callback == 0 || buf == 0 || len < 2u) {
        return;
    }

    if (len > DAB_RTLSDR_ASYNC_BUFFER_BYTES) {
        len = DAB_RTLSDR_ASYNC_BUFFER_BYTES;
    }
    sample_count = len / 2u;
    for (i = 0u; i < sample_count; ++i) {
        i_samples[i] = (int16_t)(((int)buf[2u * i] - 128) << 8);
        q_samples[i] = (int16_t)(((int)buf[(2u * i) + 1u] - 128) << 8);
    }

    adapter->callback(i_samples,
                      q_samples,
                      sample_count,
                      adapter->sample_index,
                      dab_time_monotonic_ns(),
                      adapter->user);
    adapter->sample_index += sample_count;
}

static int dab_rtlsdr_real_thread(void *arg) {
    dab_rtlsdr_real_adapter_t *adapter = (dab_rtlsdr_real_adapter_t *)arg;
    adapter->read_rc = adapter->api.read_async(adapter->dev,
                                               dab_rtlsdr_real_async_cb,
                                               adapter,
                                               DAB_RTLSDR_ASYNC_BUFFER_COUNT,
                                               DAB_RTLSDR_ASYNC_BUFFER_BYTES);
    adapter->running = 0;
    return adapter->read_rc;
}

static dab_sdr_status_t dab_rtlsdr_real_open_and_configure(dab_rtlsdr_real_adapter_t *adapter) {
    uint32_t count;
    int rc;

#if defined(__ANDROID__)
    if (adapter->base.device_config.android_usb_fd >= 0 && adapter->api.open_fd != 0) {
        rc = adapter->api.open_fd(&adapter->dev, adapter->base.device_config.android_usb_fd);
    } else
#endif
    {
#if defined(__ANDROID__)
        if (adapter->base.device_config.android_usb_fd >= 0) {
            char fd_string[32];
            snprintf(fd_string, sizeof(fd_string), "%d", adapter->base.device_config.android_usb_fd);
            setenv("RTLSDR_ANDROID_USB_FD", fd_string, 1);
            rc = adapter->api.open(&adapter->dev, adapter->base.device_index);
        } else
#endif
        {
        count = adapter->api.get_device_count();
        if (adapter->base.device_index >= count) {
            return DAB_SDR_STATUS_NOT_FOUND;
        }
        rc = adapter->api.open(&adapter->dev, adapter->base.device_index);
        }
    }
    dab_rtlsdr_debug_rc("open", rc);
    if (rc != 0 || adapter->dev == 0) {
        dab_rtlsdr_error_rc("open", rc);
        return DAB_SDR_STATUS_BUSY;
    }
    adapter->opened = 1;
    dab_rtlsdr_debug("Open ok");

    rc = adapter->api.set_freq_correction(adapter->dev, adapter->base.device_config.ppm_correction);
    dab_rtlsdr_debug_rc("set_freq_correction", rc);
    if (rc != 0) {
        dab_rtlsdr_warn_rc("set_freq_correction", rc);
    }

    rc = adapter->api.set_sample_rate(adapter->dev, adapter->base.config.sample_rate);
    dab_rtlsdr_debug_rc("set_sample_rate", rc);
    if (rc != 0) {
        dab_rtlsdr_warn_rc("set_sample_rate", rc);
    }

    rc = adapter->api.set_center_freq(adapter->dev, (uint32_t)adapter->base.config.center_freq_hz);
    dab_rtlsdr_debug_rc("set_center_freq", rc);
    if (rc != 0) {
        dab_rtlsdr_warn_rc("set_center_freq", rc);
    }

    if (adapter->api.set_tuner_bandwidth != 0) {
        rc = adapter->api.set_tuner_bandwidth(adapter->dev, adapter->base.config.bandwidth_hz);
        dab_rtlsdr_debug_rc("set_tuner_bandwidth", rc);
        if (rc != 0) {
            dab_rtlsdr_warn_rc("set_tuner_bandwidth", rc);
        }
    }

    rc = adapter->api.set_tuner_gain_mode(adapter->dev,
                                          adapter->base.device_config.gain_mode == DAB_RTLSDR_GAIN_MODE_MANUAL ? 1 : 0);
    dab_rtlsdr_debug_rc("set_tuner_gain_mode", rc);
    if (rc != 0) {
        dab_rtlsdr_warn_rc("set_tuner_gain_mode", rc);
    }
    if (adapter->base.device_config.gain_mode == DAB_RTLSDR_GAIN_MODE_MANUAL) {
        const int requested_gain = adapter->base.config.gain_db * 10;
        const int tuner_gain = dab_rtlsdr_nearest_gain(adapter, requested_gain);
        rc = adapter->api.set_tuner_gain(adapter->dev, tuner_gain);
        dab_rtlsdr_debug_rc("set_tuner_gain", rc);
        if (rc != 0) {
            dab_rtlsdr_warn_rc("set_tuner_gain", rc);
        }
    }

    if (adapter->api.set_bias_tee != 0) {
        rc = adapter->api.set_bias_tee(adapter->dev, adapter->base.device_config.bias_tee_enabled);
        dab_rtlsdr_debug_rc("set_bias_tee", rc);
        if (rc != 0) {
            dab_rtlsdr_warn_rc("set_bias_tee", rc);
        }
    }

    rc = adapter->api.reset_buffer(adapter->dev);
    dab_rtlsdr_debug_rc("reset_buffer", rc);
    if (rc != 0) {
        dab_rtlsdr_warn_rc("reset_buffer", rc);
    }
    return DAB_SDR_STATUS_OK;
}

static dab_sdr_status_t dab_rtlsdr_real_start(dab_rtlsdr_adapter_t *base,
                                              dab_rtlsdr_samples_cb_t callback,
                                              void *user) {
    dab_rtlsdr_real_adapter_t *adapter = (dab_rtlsdr_real_adapter_t *)base;
    dab_sdr_status_t status;

    if (adapter == 0 || callback == 0) {
        return DAB_SDR_STATUS_INVALID_ARGUMENT;
    }
    if (adapter->running) {
        return DAB_SDR_STATUS_BUSY;
    }

    adapter->callback = callback;
    adapter->user = user;
    adapter->sample_index = 0u;
    adapter->read_rc = 0;

    status = dab_rtlsdr_real_open_and_configure(adapter);
    if (status != DAB_SDR_STATUS_OK) {
        (void)dab_rtlsdr_real_stop(base);
        return status;
    }

    adapter->running = 1;
    if (dab_thread_create(&adapter->thread, dab_rtlsdr_real_thread, adapter, "rtlsdr-real") != 0) {
        adapter->running = 0;
        (void)dab_rtlsdr_real_stop(base);
        return DAB_SDR_STATUS_ERROR;
    }

    return DAB_SDR_STATUS_OK;
}

static dab_sdr_status_t dab_rtlsdr_real_stop(dab_rtlsdr_adapter_t *base) {
    dab_rtlsdr_real_adapter_t *adapter = (dab_rtlsdr_real_adapter_t *)base;
    dab_sdr_status_t status = DAB_SDR_STATUS_OK;
    int rc = 0;

    if (adapter == 0) {
        return DAB_SDR_STATUS_INVALID_ARGUMENT;
    }

    if (adapter->running) {
        if (adapter->api.cancel_async(adapter->dev) != 0) {
            status = DAB_SDR_STATUS_ERROR;
        }
        if (dab_thread_join(&adapter->thread, &rc) != 0) {
            status = DAB_SDR_STATUS_ERROR;
        }
        adapter->running = 0;
    }

    if (adapter->opened) {
        if (adapter->api.close(adapter->dev) != 0) {
            status = DAB_SDR_STATUS_ERROR;
        }
        adapter->dev = 0;
        adapter->opened = 0;
    }

    adapter->callback = 0;
    adapter->user = 0;
    return status;
}

static void dab_rtlsdr_real_destroy(dab_rtlsdr_adapter_t *base) {
    dab_rtlsdr_real_adapter_t *adapter = (dab_rtlsdr_real_adapter_t *)base;

    if (adapter == 0) {
        return;
    }
    if (adapter->opened || adapter->running) {
        (void)dab_rtlsdr_real_stop(base);
    }
    dab_rtlsdr_api_unload(&adapter->api);
    free(adapter);
}

static const dab_rtlsdr_adapter_vtbl_t DAB_RTLSDR_REAL_VTBL = {
    dab_rtlsdr_real_start,
    dab_rtlsdr_real_stop,
    dab_rtlsdr_real_destroy
};

dab_rtlsdr_adapter_t *dab_rtlsdr_real_adapter_create(const dab_stream_config_t *config,
                                                     const dab_rtlsdr_config_t *device_config,
                                                     uint32_t device_index,
                                                     const dab_rtlsdr_api_t *api) {
    dab_rtlsdr_real_adapter_t *adapter;

    if (config == 0 || device_config == 0 || api == 0 || !dab_rtlsdr_api_is_usable(api)) {
        return 0;
    }

    adapter = (dab_rtlsdr_real_adapter_t *)calloc(1u, sizeof(*adapter));
    if (adapter == 0) {
        return 0;
    }

    adapter->base.vtbl = &DAB_RTLSDR_REAL_VTBL;
    adapter->base.config = *config;
    adapter->base.device_config = *device_config;
    adapter->base.device_index = device_index;
    adapter->base.uses_real_api = 1;
    adapter->api = *api;
    return &adapter->base;
}
