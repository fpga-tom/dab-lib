#include "dab_gui_backend.h"

#include <pthread.h>

static pthread_mutex_t g_sdr_settings_mutex = PTHREAD_MUTEX_INITIALIZER;
static dab_gui_sdr_settings_t g_sdr_settings = {
    DAB_GUI_SDR_BACKEND_SDRPLAY,
    1,
    40
};

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

    (void)pthread_mutex_lock(&g_sdr_settings_mutex);
    g_sdr_settings = normalized;
    (void)pthread_mutex_unlock(&g_sdr_settings_mutex);
}

void dab_gui_sdr_get_settings(dab_gui_sdr_settings_t *settings) {
    if (settings == 0) {
        return;
    }

    (void)pthread_mutex_lock(&g_sdr_settings_mutex);
    *settings = g_sdr_settings;
    (void)pthread_mutex_unlock(&g_sdr_settings_mutex);
}
