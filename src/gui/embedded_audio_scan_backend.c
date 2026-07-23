#include "dab_gui_backend.h"

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <windows.h>
#endif

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

static int dab_gui_scan_cancel_callback(void *user) {
    (void)user;
    return dab_gui_scan_cancel_requested();
}

static int dab_gui_scan_event_bridge(const dab_scan_event_t *event, void *user) {
    char line[512];
    int *error_reported = (int *)user;

    if (event == 0) {
        return 0;
    }

    switch (event->type) {
        case DAB_SCAN_EVENT_PROGRESS:
            snprintf(line,
                     sizeof(line),
                     "SCAN_PROGRESS\t%u\t%u\t%s\t%llu",
                     event->index,
                     event->total,
                     event->channel != 0 ? event->channel : "",
                     (unsigned long long)event->frequency_hz);
            dab_gui_scan_emit_line(line);
            break;
        case DAB_SCAN_EVENT_LOCK:
            snprintf(line,
                     sizeof(line),
                     "SCAN_LOCK\t%u\t%u\t%s\t%llu\t%u\t%u",
                     event->index,
                     event->total,
                     event->channel != 0 ? event->channel : "",
                     (unsigned long long)event->frequency_hz,
                     event->service_count,
                     event->label_count);
            dab_gui_scan_emit_line(line);
            break;
        case DAB_SCAN_EVENT_ERROR:
            if (event->message != 0 && event->message[0] != '\0') {
                snprintf(line, sizeof(line), "SCAN_ERROR\t%s", event->message);
                dab_gui_scan_emit_line(line);
                if (error_reported != 0) {
                    *error_reported = 1;
                }
            }
            break;
        default:
            break;
    }

    return 0;
}

void dab_gui_scan_cancel_current(void) {
    dab_gui_scan_cancel_set(1);
}

static int dab_gui_scan_replace_results(const char *temporary_path, const char *output_path) {
#if defined(_WIN32)
    return MoveFileExA(temporary_path,
                       output_path,
                       MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) ? 0 : -1;
#else
    return rename(temporary_path, output_path);
#endif
}

int dab_gui_scan_run(const char *output_path, dab_gui_scan_event_callback_t callback, void *user) {
    dab_gui_sdr_settings_t settings;
    dab_gui_scan_settings_t scan_settings;
    const char *driver = "sdrplay";
    dab_sdr_open_params_t params = {"sdrplay", NULL, NULL};
    dab_sdrplay_config_t sdrplay_config;
    dab_rtlsdr_config_t rtlsdr_config;
    dab_sdr_device_t *device = 0;
    scan_runtime_t runtime = {0};
    dab_scan_config_t scan_config;
    dab_scan_context_t scan_context;
    FILE *results_fp = 0;
    char *temporary_path = 0;
    uint32_t channel_index;
    uint32_t total_services = 0u;
    uint32_t completed_channels = 0u;
    uint32_t failed_channels = 0u;
    int scan_error_reported = 0;
    int rc = -1;

    if (output_path == 0 || output_path[0] == '\0') {
        return -1;
    }
    temporary_path = (char *)malloc(strlen(output_path) + 6u);
    if (temporary_path == 0) {
        return -1;
    }
    snprintf(temporary_path, strlen(output_path) + 6u, "%s.tmp", output_path);
    (void)remove(temporary_path);

    g_scan_sink.callback = callback;
    g_scan_sink.user = user;
    dab_gui_scan_cancel_set(0);

    dab_gui_sdr_get_settings(&settings);
    dab_gui_scan_get_settings(&scan_settings);
    if (settings.backend == DAB_GUI_SDR_BACKEND_RTLSDR) {
        driver = "rtlsdr";
    }
    params.driver = driver;
    dab_scan_default_config(&scan_config);
    scan_config.driver = driver;
    scan_config.per_frequency_seconds = scan_settings.per_frequency_seconds;
    scan_config.label_wait_seconds = scan_settings.label_wait_seconds;
    scan_config.hard_channel_seconds = scan_settings.hard_channel_seconds;
    scan_config.gain_db = settings.gain_db;
    scan_config.agc_enabled = settings.agc_enabled != 0;
    scan_config.use_real_device = 1;
    scan_config.cancel = dab_gui_scan_cancel_callback;
    scan_config.cancel_user = 0;
    scan_context.config = &scan_config;
    scan_context.callback = dab_gui_scan_event_bridge;
    scan_context.user = &scan_error_reported;

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

    results_fp = fopen(temporary_path, "wb");
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
                           scan_settings.per_frequency_seconds,
                           scan_settings.label_wait_seconds,
                           scan_settings.hard_channel_seconds,
                           settings.gain_db,
                           results_fp,
                           &scan_context);
        if (services == -2) {
            rc = -2;
            goto cleanup;
        }
        if (services < 0) {
            if (scan_error_reported == 0) {
                char line[128];
                snprintf(line, sizeof(line), "SCAN_ERROR\tchannel %s failed", kBandIIIChannels[channel_index].channel);
                dab_gui_scan_emit_line(line);
            }
            failed_channels++;
            scan_error_reported = 0;
            continue;
        }
        completed_channels++;
        total_services += (uint32_t)services;
    }
    if (completed_channels == 0u) {
        dab_gui_scan_emit_line("SCAN_ERROR\tall DAB channels failed");
        goto cleanup;
    }
    if (dab_gui_scan_cancel_requested()) {
        rc = -2;
        goto cleanup;
    }
    if (fclose(results_fp) != 0) {
        results_fp = 0;
        dab_gui_scan_emit_line("SCAN_ERROR\tfailed to finalize scan results");
        goto cleanup;
    }
    results_fp = 0;
    if (dab_gui_scan_replace_results(temporary_path, output_path) != 0) {
        dab_gui_scan_emit_line("SCAN_ERROR\tfailed to replace scan results");
        goto cleanup;
    }

    if (callback != 0) {
        char complete[512];
        snprintf(complete,
                 sizeof(complete),
                 "SCAN_COMPLETE\t%s\t%u\t%u",
                 output_path,
                 total_services,
                 failed_channels);
        callback(complete, user);
    }
    rc = 0;

cleanup:
    if (results_fp != 0) {
        fclose(results_fp);
    }
    if (rc != 0 && temporary_path != 0) {
        (void)remove(temporary_path);
    }
    if (device != 0) {
        dab_sdr_device_close(device);
    }
    scan_runtime_destroy(&runtime);
    g_scan_sink = (dab_gui_scan_sink_t){0};
    dab_gui_scan_cancel_set(0);
    free(temporary_path);
    return rc;
}
