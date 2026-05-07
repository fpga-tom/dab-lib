#ifndef DAB_GUI_BACKEND_H
#define DAB_GUI_BACKEND_H

#include <stdint.h>

#include "dab/core/audio_spectrum.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*dab_gui_scan_event_callback_t)(const char *line, void *user);

typedef struct {
    int running;
    int locked;
    int has_receiver_snr;
    float receiver_snr_db;
    int has_spectrum_cn;
    float spectrum_cn_db;
    float prs_score;
    uint64_t frequency_hz;
    uint32_t sid;
} dab_gui_playback_status_t;

typedef struct {
    int valid;
    uint32_t sample_rate;
    uint32_t bin_count;
    float bins[DAB_AUDIO_SPECTRUM_DISPLAY_BINS];
} dab_gui_playback_spectrum_t;

enum {
    DAB_GUI_PLAYBACK_LOGO_MAX_BYTES = 256u * 1024u
};

typedef struct {
    int valid;
    uint64_t generation;
    uint32_t byte_count;
    uint8_t bytes[DAB_GUI_PLAYBACK_LOGO_MAX_BYTES];
} dab_gui_playback_logo_t;

typedef enum {
    DAB_GUI_SDR_BACKEND_SDRPLAY = 0,
    DAB_GUI_SDR_BACKEND_RTLSDR = 1
} dab_gui_sdr_backend_t;

typedef struct {
    dab_gui_sdr_backend_t backend;
    int agc_enabled;
    int gain_db;
} dab_gui_sdr_settings_t;

void dab_gui_sdr_default_settings(dab_gui_sdr_settings_t *settings);
void dab_gui_sdr_set_settings(const dab_gui_sdr_settings_t *settings);
void dab_gui_sdr_get_settings(dab_gui_sdr_settings_t *settings);

int dab_gui_scan_run(const char *output_path, dab_gui_scan_event_callback_t callback, void *user);
void dab_gui_scan_cancel_current(void);

int dab_gui_playback_start(uint64_t frequency_hz, uint32_t sid);
void dab_gui_playback_stop(void);
int dab_gui_playback_is_running(void);
int dab_gui_playback_get_status(dab_gui_playback_status_t *status);
int dab_gui_playback_get_spectrum(dab_gui_playback_spectrum_t *spectrum);
int dab_gui_playback_get_logo(dab_gui_playback_logo_t *logo);
int dab_gui_playback_set_equalizer(const float *gains_db, uint32_t count);

#ifdef __cplusplus
}
#endif

#endif
