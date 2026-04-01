#ifndef DAB_GUI_BACKEND_H
#define DAB_GUI_BACKEND_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*dab_gui_scan_event_callback_t)(const char *line, void *user);

int dab_gui_scan_run(const char *output_path, dab_gui_scan_event_callback_t callback, void *user);

int dab_gui_playback_start(uint64_t frequency_hz, uint32_t sid);
void dab_gui_playback_stop(void);
int dab_gui_playback_is_running(void);

#ifdef __cplusplus
}
#endif

#endif
