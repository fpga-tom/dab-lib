#include "dab_gui_backend.h"

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../include/dab/hal/sdr_device.h"
#include "../../include/dab/hal/sdrplay_device.h"

typedef struct {
    dab_gui_scan_event_callback_t callback;
    void *user;
} dab_gui_scan_sink_t;

static dab_gui_scan_sink_t g_scan_sink = {0};

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
#include "../../tools/dab_sdrplay_scan.c"
#undef main
#undef printf

int dab_gui_scan_run(const char *output_path, dab_gui_scan_event_callback_t callback, void *user) {
    dab_sdr_open_params_t params = {"sdrplay", NULL, NULL};
    dab_sdrplay_config_t device_config;
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

    setenv("DAB_SDRPLAY_USE_REAL", "1", 1);

    dab_sdrplay_default_config(&device_config);
    device_config.gain_mode = DAB_SDRPLAY_GAIN_MODE_AUTO;
    device_config.if_mode = DAB_SDRPLAY_IF_MODE_ZERO_IF;
    device_config.antenna = DAB_SDRPLAY_ANTENNA_A;

    if (scan_runtime_init(&runtime, 2048000u) != 0) {
        dab_gui_scan_emit_line("SCAN_ERROR\tfailed to initialize scan runtime");
        goto cleanup;
    }

    if (dab_sdr_device_open(&params, &device) != DAB_SDR_STATUS_OK || device == 0) {
        dab_gui_scan_emit_line("SCAN_ERROR\tfailed to open SDRplay device");
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
        const int services = scan_frequency(&runtime,
                                            device,
                                            &device_config,
                                            &kBandIIIChannels[channel_index],
                                            channel_index,
                                            4u,
                                            results_fp);
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
    return rc;
}
