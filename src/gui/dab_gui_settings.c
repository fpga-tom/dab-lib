#include "dab_gui_backend.h"

#include "dab/os/os_mutex.h"

static dab_mutex_t g_sdr_settings_mutex;
static int g_sdr_settings_mutex_ready = 0;
static dab_gui_sdr_settings_t g_sdr_settings = {
    DAB_GUI_SDR_BACKEND_SDRPLAY,
    1,
    40
};
static dab_mutex_t g_audio_settings_mutex;
static int g_audio_settings_mutex_ready = 0;
static dab_gui_audio_settings_t g_audio_settings = {
    DAB_AUDIO_OUTPUT_AUTO,
    0
};
static dab_mutex_t g_scan_settings_mutex;
static int g_scan_settings_mutex_ready = 0;
static dab_gui_scan_settings_t g_scan_settings = {
    DAB_SCAN_DEFAULT_PER_FREQUENCY_SECONDS,
    DAB_SCAN_DEFAULT_LABEL_WAIT_SECONDS,
    DAB_SCAN_DEFAULT_HARD_CHANNEL_SECONDS
};

static void dab_gui_sdr_settings_ensure_mutex(void) {
    if (g_sdr_settings_mutex_ready == 0) {
        (void)dab_mutex_init(&g_sdr_settings_mutex);
        g_sdr_settings_mutex_ready = 1;
    }
}

static void dab_gui_audio_settings_ensure_mutex(void) {
    if (g_audio_settings_mutex_ready == 0) {
        (void)dab_mutex_init(&g_audio_settings_mutex);
        g_audio_settings_mutex_ready = 1;
    }
}

static void dab_gui_scan_settings_ensure_mutex(void) {
    if (g_scan_settings_mutex_ready == 0) {
        (void)dab_mutex_init(&g_scan_settings_mutex);
        g_scan_settings_mutex_ready = 1;
    }
}

void dab_gui_sdr_default_settings(dab_gui_sdr_settings_t *settings) {
    if (settings == 0) {
        return;
    }
    settings->backend = DAB_GUI_SDR_BACKEND_SDRPLAY;
    settings->agc_enabled = 1;
    settings->gain_db = 40;
}

void dab_gui_sdr_set_settings(const dab_gui_sdr_settings_t *settings) {
    dab_gui_sdr_settings_t normalized;

    if (settings == 0) {
        return;
    }

    normalized = *settings;
    if (normalized.backend != DAB_GUI_SDR_BACKEND_SDRPLAY &&
        normalized.backend != DAB_GUI_SDR_BACKEND_RTLSDR) {
        normalized.backend = DAB_GUI_SDR_BACKEND_SDRPLAY;
    }
    normalized.agc_enabled = normalized.agc_enabled != 0 ? 1 : 0;
    if (normalized.gain_db < 0) {
        normalized.gain_db = 0;
    }
    if (normalized.gain_db > 59) {
        normalized.gain_db = 59;
    }
    if (normalized.backend == DAB_GUI_SDR_BACKEND_RTLSDR && normalized.gain_db > 49) {
        normalized.gain_db = 49;
    }

    dab_gui_sdr_settings_ensure_mutex();
    dab_mutex_lock(&g_sdr_settings_mutex);
    g_sdr_settings = normalized;
    dab_mutex_unlock(&g_sdr_settings_mutex);
}

void dab_gui_sdr_get_settings(dab_gui_sdr_settings_t *settings) {
    if (settings == 0) {
        return;
    }

    dab_gui_sdr_settings_ensure_mutex();
    dab_mutex_lock(&g_sdr_settings_mutex);
    *settings = g_sdr_settings;
    dab_mutex_unlock(&g_sdr_settings_mutex);
}

void dab_gui_audio_default_settings(dab_gui_audio_settings_t *settings) {
    if (settings == 0) {
        return;
    }
    settings->backend = DAB_AUDIO_OUTPUT_AUTO;
    settings->realtime_decoding = 0;
}

void dab_gui_audio_set_settings(const dab_gui_audio_settings_t *settings) {
    dab_gui_audio_settings_t normalized;

    if (settings == 0) {
        return;
    }
    normalized = *settings;
    if (normalized.backend < DAB_AUDIO_OUTPUT_AUTO ||
        normalized.backend > DAB_AUDIO_OUTPUT_WASAPI ||
        (normalized.backend != DAB_AUDIO_OUTPUT_AUTO &&
         dab_audio_output_backend_available(normalized.backend) == 0)) {
        normalized.backend = DAB_AUDIO_OUTPUT_AUTO;
    }
    normalized.realtime_decoding = normalized.realtime_decoding != 0;
    dab_gui_audio_settings_ensure_mutex();
    dab_mutex_lock(&g_audio_settings_mutex);
    g_audio_settings = normalized;
    dab_mutex_unlock(&g_audio_settings_mutex);
}

void dab_gui_audio_get_settings(dab_gui_audio_settings_t *settings) {
    if (settings == 0) {
        return;
    }
    dab_gui_audio_settings_ensure_mutex();
    dab_mutex_lock(&g_audio_settings_mutex);
    *settings = g_audio_settings;
    dab_mutex_unlock(&g_audio_settings_mutex);
}

void dab_gui_scan_default_settings(dab_gui_scan_settings_t *settings) {
    if (settings == 0) {
        return;
    }
    settings->per_frequency_seconds = DAB_SCAN_DEFAULT_PER_FREQUENCY_SECONDS;
    settings->label_wait_seconds = DAB_SCAN_DEFAULT_LABEL_WAIT_SECONDS;
    settings->hard_channel_seconds = DAB_SCAN_DEFAULT_HARD_CHANNEL_SECONDS;
}

void dab_gui_scan_set_settings(const dab_gui_scan_settings_t *settings) {
    dab_scan_config_t normalized = {0};

    if (settings == 0) {
        return;
    }
    normalized.per_frequency_seconds = settings->per_frequency_seconds;
    normalized.label_wait_seconds = settings->label_wait_seconds;
    normalized.hard_channel_seconds = settings->hard_channel_seconds;
    dab_scan_normalize_config(&normalized);

    dab_gui_scan_settings_ensure_mutex();
    dab_mutex_lock(&g_scan_settings_mutex);
    g_scan_settings.per_frequency_seconds = normalized.per_frequency_seconds;
    g_scan_settings.label_wait_seconds = normalized.label_wait_seconds;
    g_scan_settings.hard_channel_seconds = normalized.hard_channel_seconds;
    dab_mutex_unlock(&g_scan_settings_mutex);
}

void dab_gui_scan_get_settings(dab_gui_scan_settings_t *settings) {
    if (settings == 0) {
        return;
    }
    dab_gui_scan_settings_ensure_mutex();
    dab_mutex_lock(&g_scan_settings_mutex);
    *settings = g_scan_settings;
    dab_mutex_unlock(&g_scan_settings_mutex);
}
