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

int dab_gui_scan_run(const char *output_path, dab_gui_scan_event_callback_t callback, void *user);

int dab_gui_playback_start(uint64_t frequency_hz, uint32_t sid);
void dab_gui_playback_stop(void);
int dab_gui_playback_is_running(void);
int dab_gui_playback_get_status(dab_gui_playback_status_t *status);
int dab_gui_playback_get_spectrum(dab_gui_playback_spectrum_t *spectrum);
int dab_gui_playback_set_equalizer(const float *gains_db, uint32_t count);

#ifdef __cplusplus
}
#endif

#endif
