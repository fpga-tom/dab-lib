#include "sdrplay_adapter.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dab/os/os_time.h"

enum {
    DAB_SDRPLAY_MAX_DEVICES = 8
};

typedef struct {
    dab_sdrplay_adapter_t base;
    dab_sdrplay_api_t api;
    dab_sdrplay_api_device_t selected_device;
    dab_sdrplay_device_params_t *device_params;
    dab_sdrplay_api_callbacks_t callbacks;
    dab_sdrplay_samples_cb_t callback;
    void *user;
    float api_version_value;
    int opened;
    int selected;
    int initialized;
    int started;
} dab_sdrplay_real_adapter_t;

static dab_sdr_status_t dab_sdrplay_real_stop(dab_sdrplay_adapter_t *base);

static int dab_sdrplay_debug_enabled(void) {
    const char *value = getenv("DAB_SDRPLAY_DEBUG");
    return value != 0 && value[0] == '1';
}

static void dab_sdrplay_debug(const char *msg) {
    if (dab_sdrplay_debug_enabled()) {
        fprintf(stderr, "sdrplay-real: %s\n", msg);
        fflush(stderr);
    }
}

static dab_sdrplay_bw_mhz_t dab_sdrplay_map_bandwidth(uint32_t bandwidth_hz) {
    if (bandwidth_hz <= 200000u) {
        return DAB_SDRPLAY_BW_0_200;
    }
    if (bandwidth_hz <= 300000u) {
        return DAB_SDRPLAY_BW_0_300;
    }
    if (bandwidth_hz <= 600000u) {
        return DAB_SDRPLAY_BW_0_600;
    }
    if (bandwidth_hz <= 1536000u) {
        return DAB_SDRPLAY_BW_1_536;
    }
    if (bandwidth_hz <= 5000000u) {
        return DAB_SDRPLAY_BW_5_000;
    }
    if (bandwidth_hz <= 6000000u) {
        return DAB_SDRPLAY_BW_6_000;
    }
    if (bandwidth_hz <= 7000000u) {
        return DAB_SDRPLAY_BW_7_000;
    }
    return DAB_SDRPLAY_BW_8_000;
}

static dab_sdrplay_if_khz_t dab_sdrplay_map_if_mode(dab_sdrplay_if_mode_t if_mode) {
    return if_mode == DAB_SDRPLAY_IF_MODE_LOW_IF ? DAB_SDRPLAY_IF_0_450 : DAB_SDRPLAY_IF_ZERO;
}

static dab_sdrplay_rspdx_antenna_select_t dab_sdrplay_map_rspdx_antenna(dab_sdrplay_antenna_t antenna) {
    if (antenna == DAB_SDRPLAY_ANTENNA_B) {
        return DAB_SDRPLAY_RSPDX_ANTENNA_B;
    }
    return DAB_SDRPLAY_RSPDX_ANTENNA_A;
}

static dab_sdrplay_rsp2_antenna_select_t dab_sdrplay_map_rsp2_antenna(dab_sdrplay_antenna_t antenna) {
    if (antenna == DAB_SDRPLAY_ANTENNA_B) {
        return DAB_SDRPLAY_RSP2_ANTENNA_B;
    }
    return DAB_SDRPLAY_RSP2_ANTENNA_A;
}

static void dab_sdrplay_apply_stream_params(dab_sdrplay_real_adapter_t *adapter) {
    dab_sdrplay_dev_params_t *dev_params = adapter->device_params->devParams;
    dab_sdrplay_rx_channel_params_t *rx = adapter->device_params->rxChannelA;

    dev_params->ppm = (double)adapter->base.device_config.ppm_correction;
    dev_params->fsFreq.fsHz = (double)adapter->base.config.sample_rate;

    rx->tunerParams.rfFreq.rfHz = (double)adapter->base.config.center_freq_hz;
    rx->tunerParams.bwType = dab_sdrplay_map_bandwidth(adapter->base.config.bandwidth_hz);
    rx->tunerParams.ifType = dab_sdrplay_map_if_mode(adapter->base.device_config.if_mode);
    rx->tunerParams.loMode = DAB_SDRPLAY_LO_AUTO;
    rx->tunerParams.gain.gRdB = adapter->base.config.gain_db;
    rx->tunerParams.gain.LNAstate = adapter->base.device_config.lna_state;
    rx->tunerParams.gain.minGr = DAB_SDRPLAY_NORMAL_MIN_GR;

    rx->ctrlParams.agc.enable = adapter->base.device_config.gain_mode == DAB_SDRPLAY_GAIN_MODE_AUTO ?
        DAB_SDRPLAY_AGC_50HZ : DAB_SDRPLAY_AGC_DISABLE;
    rx->ctrlParams.dcOffset.DCenable = 1u;
    rx->ctrlParams.dcOffset.IQenable = 1u;

    dev_params->rsp1aParams.rfDabNotchEnable = 1u;
    rx->rsp1aTunerParams.biasTEnable = (unsigned char)adapter->base.device_config.bias_tee_enabled;

    rx->rsp2TunerParams.biasTEnable = (unsigned char)adapter->base.device_config.bias_tee_enabled;
    rx->rsp2TunerParams.antennaSel = dab_sdrplay_map_rsp2_antenna(adapter->base.device_config.antenna);

    rx->rspDuoTunerParams.biasTEnable = (unsigned char)adapter->base.device_config.bias_tee_enabled;
    rx->rspDuoTunerParams.rfDabNotchEnable = 1u;

    dev_params->rspDxParams.biasTEnable = (unsigned char)adapter->base.device_config.bias_tee_enabled;
    dev_params->rspDxParams.antennaSel = dab_sdrplay_map_rspdx_antenna(adapter->base.device_config.antenna);
    dev_params->rspDxParams.rfDabNotchEnable = 1u;
}

static dab_sdr_status_t dab_sdrplay_apply_updates(dab_sdrplay_real_adapter_t *adapter) {
    dab_sdrplay_reason_for_update_t reason = DAB_SDRPLAY_UPDATE_DEV_FS |
        DAB_SDRPLAY_UPDATE_DEV_PPM |
        DAB_SDRPLAY_UPDATE_TUNER_GR |
        DAB_SDRPLAY_UPDATE_TUNER_FRF |
        DAB_SDRPLAY_UPDATE_TUNER_BWTYPE |
        DAB_SDRPLAY_UPDATE_TUNER_IFTYPE |
        DAB_SDRPLAY_UPDATE_CTRL_AGC;
    dab_sdrplay_reason_for_update_ext1_t reason_ext1 = DAB_SDRPLAY_UPDATE_EXT1_NONE;

    if (adapter->selected_device.hwVer == 4u || adapter->selected_device.hwVer == 7u) {
        reason_ext1 = DAB_SDRPLAY_UPDATE_RSPDX_BIAS_T |
            DAB_SDRPLAY_UPDATE_RSPDX_ANTENNA |
            DAB_SDRPLAY_UPDATE_RSPDX_RF_DAB_NOTCH;
    } else if (adapter->selected_device.hwVer == 2u) {
        reason |= DAB_SDRPLAY_UPDATE_RSP2_BIAS_T | DAB_SDRPLAY_UPDATE_RSP2_ANTENNA;
    } else if (adapter->selected_device.hwVer == 3u) {
        reason |= DAB_SDRPLAY_UPDATE_RSPDUO_BIAS_T | DAB_SDRPLAY_UPDATE_RSPDUO_RF_DAB_NOTCH;
    } else {
        reason |= DAB_SDRPLAY_UPDATE_RSP1A_BIAS_T | DAB_SDRPLAY_UPDATE_RSP1A_RF_DAB_NOTCH;
    }

    if (adapter->api.update(adapter->selected_device.dev, DAB_SDRPLAY_TUNER_A, reason, reason_ext1) != 0) {
        return DAB_SDR_STATUS_ERROR;
    }
    return DAB_SDR_STATUS_OK;
}

static int dab_sdrplay_startup_update_enabled(void) {
    const char *value = getenv("DAB_SDRPLAY_STARTUP_UPDATE");
    return value != 0 && value[0] == '1';
}

static void dab_sdrplay_real_stream_a_cb(short *xi,
                                         short *xq,
                                         dab_sdrplay_stream_cb_params_t *params,
                                         unsigned int num_samples,
                                         uint32_t reset,
                                         void *cb_context) {
    dab_sdrplay_real_adapter_t *adapter = (dab_sdrplay_real_adapter_t *)cb_context;

    (void)reset;

    if (adapter == 0 || adapter->callback == 0 || xi == 0 || xq == 0 || num_samples == 0u) {
        return;
    }

    adapter->callback(xi,
                      xq,
                      num_samples,
                      params != 0 ? params->firstSampleNum : 0u,
                      dab_time_monotonic_ns(),
                      adapter->user);
}

static void dab_sdrplay_real_event_cb(uint32_t event_id,
                                      uint32_t tuner,
                                      void *params,
                                      void *cb_context) {
    (void)event_id;
    (void)tuner;
    (void)params;
    (void)cb_context;
}

static dab_sdr_status_t dab_sdrplay_real_open_and_select(dab_sdrplay_real_adapter_t *adapter) {
    dab_sdrplay_api_device_t devices[DAB_SDRPLAY_MAX_DEVICES];
    unsigned int count = DAB_SDRPLAY_MAX_DEVICES;

    if (adapter->api.open_api() != 0) {
        return DAB_SDR_STATUS_ERROR;
    }
    dab_sdrplay_debug("Open ok");
    adapter->opened = 1;

    if (adapter->api.api_version != 0) {
        (void)adapter->api.api_version(&adapter->api_version_value);
    }

    memset(devices, 0, sizeof(devices));
    dab_sdrplay_debug("GetDevices");
    if (adapter->api.get_devices(devices, &count, DAB_SDRPLAY_MAX_DEVICES) != 0 || count == 0u) {
        return DAB_SDR_STATUS_NOT_FOUND;
    }
    dab_sdrplay_debug("GetDevices ok");

    adapter->selected_device = devices[0];
    dab_sdrplay_debug("SelectDevice");
    if (adapter->api.select_device(&adapter->selected_device) != 0) {
        return DAB_SDR_STATUS_BUSY;
    }
    dab_sdrplay_debug("SelectDevice ok");
    adapter->selected = 1;
    return DAB_SDR_STATUS_OK;
}

static dab_sdr_status_t dab_sdrplay_real_init_stream(dab_sdrplay_real_adapter_t *adapter) {
    dab_sdrplay_debug("GetDeviceParams");
    if (adapter->api.get_device_params(adapter->selected_device.dev, &adapter->device_params) != 0 ||
        adapter->device_params == 0 ||
        adapter->device_params->devParams == 0 ||
        adapter->device_params->rxChannelA == 0) {
        return DAB_SDR_STATUS_ERROR;
    }
    dab_sdrplay_debug("GetDeviceParams ok");

    dab_sdrplay_apply_stream_params(adapter);

    memset(&adapter->callbacks, 0, sizeof(adapter->callbacks));
    adapter->callbacks.stream_a_cb = dab_sdrplay_real_stream_a_cb;
    adapter->callbacks.event_cb = dab_sdrplay_real_event_cb;

    dab_sdrplay_debug("Init");
    if (adapter->api.init(adapter->selected_device.dev, &adapter->callbacks, adapter) != 0) {
        return DAB_SDR_STATUS_ERROR;
    }
    dab_sdrplay_debug("Init ok");
    adapter->initialized = 1;
    if (dab_sdrplay_startup_update_enabled()) {
        dab_sdrplay_debug("Update");
        return dab_sdrplay_apply_updates(adapter);
    }
    return DAB_SDR_STATUS_OK;
}

static dab_sdr_status_t dab_sdrplay_real_start(dab_sdrplay_adapter_t *base,
                                               dab_sdrplay_samples_cb_t callback,
                                               void *user) {
    dab_sdrplay_real_adapter_t *adapter = (dab_sdrplay_real_adapter_t *)base;
    dab_sdr_status_t status;

    if (adapter == 0 || callback == 0) {
        return DAB_SDR_STATUS_INVALID_ARGUMENT;
    }
    if (adapter->started) {
        return DAB_SDR_STATUS_BUSY;
    }
    adapter->callback = callback;
    adapter->user = user;

    status = dab_sdrplay_real_open_and_select(adapter);
    if (status != DAB_SDR_STATUS_OK) {
        (void)dab_sdrplay_real_stop(base);
        return status;
    }

    status = dab_sdrplay_real_init_stream(adapter);
    if (status != DAB_SDR_STATUS_OK) {
        (void)dab_sdrplay_real_stop(base);
        return status;
    }

    adapter->started = 1;
    return DAB_SDR_STATUS_OK;
}

static dab_sdr_status_t dab_sdrplay_real_stop(dab_sdrplay_adapter_t *base) {
    dab_sdrplay_real_adapter_t *adapter = (dab_sdrplay_real_adapter_t *)base;
    dab_sdr_status_t status = DAB_SDR_STATUS_OK;

    if (adapter == 0) {
        return DAB_SDR_STATUS_INVALID_ARGUMENT;
    }

    if (adapter->initialized) {
        if (adapter->api.uninit(adapter->selected_device.dev) != 0) {
            status = DAB_SDR_STATUS_ERROR;
        }
        adapter->initialized = 0;
    }

    if (adapter->selected) {
        if (adapter->api.release_device(&adapter->selected_device) != 0) {
            status = DAB_SDR_STATUS_ERROR;
        }
        adapter->selected = 0;
        memset(&adapter->selected_device, 0, sizeof(adapter->selected_device));
    }

    adapter->device_params = 0;

    if (adapter->opened) {
        if (adapter->api.close_api() != 0) {
            status = DAB_SDR_STATUS_ERROR;
        }
        adapter->opened = 0;
    }

    adapter->callback = 0;
    adapter->user = 0;
    adapter->started = 0;
    return status;
}

static void dab_sdrplay_real_destroy(dab_sdrplay_adapter_t *base) {
    dab_sdrplay_real_adapter_t *adapter = (dab_sdrplay_real_adapter_t *)base;

    if (adapter == 0) {
        return;
    }
    if (adapter->opened) {
        (void)dab_sdrplay_real_stop(base);
    }
    dab_sdrplay_api_unload(&adapter->api);
    free(adapter);
}

static const dab_sdrplay_adapter_vtbl_t DAB_SDRPLAY_REAL_VTBL = {
    dab_sdrplay_real_start,
    dab_sdrplay_real_stop,
    dab_sdrplay_real_destroy
};

dab_sdrplay_adapter_t *dab_sdrplay_real_adapter_create(const dab_stream_config_t *config,
                                                       const dab_sdrplay_config_t *device_config,
                                                       const dab_sdrplay_api_t *api) {
    dab_sdrplay_real_adapter_t *adapter;

    if (config == 0 || device_config == 0 || api == 0 || !dab_sdrplay_api_is_usable(api)) {
        return 0;
    }

    adapter = (dab_sdrplay_real_adapter_t *)calloc(1u, sizeof(*adapter));
    if (adapter == 0) {
        return 0;
    }

    adapter->base.vtbl = &DAB_SDRPLAY_REAL_VTBL;
    adapter->base.config = *config;
    adapter->base.device_config = *device_config;
    adapter->base.uses_real_api = 1;
    adapter->api = *api;
    return &adapter->base;
}
