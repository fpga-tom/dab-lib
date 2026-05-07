#include "dab_gui_backend.h"

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../include/dab/hal/sdr_device.h"
#include "../../include/dab/hal/rtlsdr_device.h"
#include "../../include/dab/hal/sdrplay_device.h"
#include "../../include/dab/os/os_mutex.h"

typedef struct {
    dab_gui_scan_event_callback_t callback;
    void *user;
} dab_gui_scan_sink_t;

static dab_gui_scan_sink_t g_scan_sink = {0};
static dab_mutex_t g_scan_cancel_mutex;
static int g_scan_cancel_mutex_ready = 0;
static int g_scan_cancel_requested = 0;

static void dab_gui_scan_cancel_ensure_mutex(void) {
    if (g_scan_cancel_mutex_ready == 0) {
        (void)dab_mutex_init(&g_scan_cancel_mutex);
        g_scan_cancel_mutex_ready = 1;
    }
}

static void dab_gui_scan_cancel_set(int requested) {
    dab_gui_scan_cancel_ensure_mutex();
    dab_mutex_lock(&g_scan_cancel_mutex);
    g_scan_cancel_requested = requested != 0 ? 1 : 0;
    dab_mutex_unlock(&g_scan_cancel_mutex);
}

static int dab_gui_scan_cancel_requested(void) {
    int requested;
    dab_gui_scan_cancel_ensure_mutex();
    dab_mutex_lock(&g_scan_cancel_mutex);
    requested = g_scan_cancel_requested;
    dab_mutex_unlock(&g_scan_cancel_mutex);
    return requested;
}

static void dab_gui_setenv_value(const char *name, const char *value) {
#if defined(_WIN32)
    (void)_putenv_s(name, value);
#else
    (void)setenv(name, value, 1);
#endif
}

static void dab_gui_setenv_real_backend(void) {
    dab_gui_setenv_value("DAB_SDRPLAY_USE_REAL", "1");
    dab_gui_setenv_value("DAB_RTLSDR_USE_REAL", "1");
}

static void dab_gui_scan_emit_line(const char *line) {
    if (line != 0 && line[0] != '\0' && g_scan_sink.callback != 0) {
        g_scan_sink.callback(line, g_scan_sink.user);
    }
}

static int dab_gui_scan_vprintf(const char *fmt, va_list args) {
    char buffer[2048];
    const int written = vsnprintf(buffer, sizeof(buffer), fmt, args);
    size_t offset = 0u;

    if (written < 0) {
        return written;
    }

    while (buffer[offset] != '\0') {
        char *newline = strchr(buffer + offset, '\n');
        if (newline != 0) {
            *newline = '\0';
        }
        dab_gui_scan_emit_line(buffer + offset);
        if (newline == 0) {
            break;
        }
        offset = (size_t)(newline - buffer) + 1u;
    }

    return written;
}

static int dab_gui_scan_printf(const char *fmt, ...) {
    va_list args;
    int rc;

    va_start(args, fmt);
    rc = dab_gui_scan_vprintf(fmt, args);
    va_end(args);
    return rc;
}

#define printf dab_gui_scan_printf
#define main dab_gui_embedded_scan_tool_main
#define DAB_SCAN_CANCEL_REQUESTED() dab_gui_scan_cancel_requested()
#include "../../tools/dab_sdrplay_scan.c"
#undef DAB_SCAN_CANCEL_REQUESTED
#undef main
#undef printf

void dab_gui_scan_cancel_current(void) {
    dab_gui_scan_cancel_set(1);
}

int dab_gui_scan_run(const char *output_path, dab_gui_scan_event_callback_t callback, void *user) {
    dab_gui_sdr_settings_t settings;
    const char *driver = "sdrplay";
    dab_sdr_open_params_t params = {"sdrplay", NULL, NULL};
    dab_sdrplay_config_t sdrplay_config;
    dab_rtlsdr_config_t rtlsdr_config;
    dab_sdr_device_t *device = 0;
    scan_runtime_t runtime = {0};
    FILE *results_fp = 0;
    uint32_t channel_index;
    uint32_t total_services = 0u;
    int rc = -1;

    if (output_path == 0 || output_path[0] == '\0') {
        return -1;
    }

    g_scan_sink.callback = callback;
    g_scan_sink.user = user;
    dab_gui_scan_cancel_set(0);

    dab_gui_sdr_get_settings(&settings);
    if (settings.backend == DAB_GUI_SDR_BACKEND_RTLSDR) {
        driver = "rtlsdr";
    }
    params.driver = driver;

    dab_gui_setenv_real_backend();

    dab_sdrplay_default_config(&sdrplay_config);
    sdrplay_config.gain_mode = settings.agc_enabled != 0 ?
        DAB_SDRPLAY_GAIN_MODE_AUTO : DAB_SDRPLAY_GAIN_MODE_MANUAL;
    sdrplay_config.if_mode = DAB_SDRPLAY_IF_MODE_ZERO_IF;
    sdrplay_config.antenna = DAB_SDRPLAY_ANTENNA_A;

    dab_rtlsdr_default_config(&rtlsdr_config);
    rtlsdr_config.gain_mode = settings.agc_enabled != 0 ?
        DAB_RTLSDR_GAIN_MODE_AUTO : DAB_RTLSDR_GAIN_MODE_MANUAL;

    if (scan_runtime_init(&runtime, 2048000u) != 0) {
        dab_gui_scan_emit_line("SCAN_ERROR\tfailed to initialize scan runtime");
        goto cleanup;
    }

    if (dab_sdr_device_open(&params, &device) != DAB_SDR_STATUS_OK || device == 0) {
        if (settings.backend == DAB_GUI_SDR_BACKEND_RTLSDR) {
            dab_gui_scan_emit_line("SCAN_ERROR\tfailed to open RTL-SDR device");
        } else {
            dab_gui_scan_emit_line("SCAN_ERROR\tfailed to open SDRplay device");
        }
        goto cleanup;
    }

    results_fp = fopen(output_path, "wb");
    if (results_fp == 0) {
        dab_gui_scan_emit_line("SCAN_ERROR\tfailed to open scan results file");
        goto cleanup;
    }
    if (fprintf(results_fp, "channel\tfrequency_hz\tservice_id\tlabel\n") < 0) {
        dab_gui_scan_emit_line("SCAN_ERROR\tfailed to write scan results header");
        goto cleanup;
    }

    for (channel_index = 0u; channel_index < (uint32_t)DAB_CHANNEL_COUNT; ++channel_index) {
        const int services = dab_gui_scan_cancel_requested() ? -2 :
            scan_frequency(&runtime,
                           device,
                           driver,
                           &sdrplay_config,
                           &rtlsdr_config,
                           &kBandIIIChannels[channel_index],
                           channel_index,
                           4u,
                           settings.gain_db,
                           results_fp);
        if (services == -2) {
            rc = -2;
            goto cleanup;
        }
        if (services < 0) {
            char line[128];
            snprintf(line, sizeof(line), "SCAN_ERROR\tchannel %s failed", kBandIIIChannels[channel_index].channel);
            dab_gui_scan_emit_line(line);
            goto cleanup;
        }
        total_services += (uint32_t)services;
    }

    if (callback != 0) {
        char complete[512];
        snprintf(complete, sizeof(complete), "SCAN_COMPLETE\t%s\t%u", output_path, total_services);
        callback(complete, user);
    }
    rc = 0;

cleanup:
    if (results_fp != 0) {
        fclose(results_fp);
    }
    if (device != 0) {
        dab_sdr_device_close(device);
    }
    scan_runtime_destroy(&runtime);
    g_scan_sink = (dab_gui_scan_sink_t){0};
    dab_gui_scan_cancel_set(0);
    return rc;
}
