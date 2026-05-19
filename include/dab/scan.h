#ifndef DAB_SCAN_H
#define DAB_SCAN_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    DAB_SCAN_EVENT_PROGRESS = 1,
    DAB_SCAN_EVENT_LOCK = 2,
    DAB_SCAN_EVENT_SERVICE = 3,
    DAB_SCAN_EVENT_COMPLETE = 4,
    DAB_SCAN_EVENT_ERROR = 5
} dab_scan_event_type_t;

typedef struct {
    dab_scan_event_type_t type;
    const char *channel;
    uint64_t frequency_hz;
    uint32_t index;
    uint32_t total;
    uint32_t service_id;
    const char *label;
    uint32_t service_count;
    uint32_t label_count;
    const char *message;
} dab_scan_event_t;

typedef int (*dab_scan_callback_t)(const dab_scan_event_t *event, void *user);
typedef int (*dab_scan_cancel_callback_t)(void *user);

typedef struct {
    const char *driver;
    uint32_t per_frequency_seconds;
    int gain_db;
    int agc_enabled;
    int ppm_correction;
    int bias_tee_enabled;
    int use_real_device;
    dab_scan_cancel_callback_t cancel;
    void *cancel_user;
} dab_scan_config_t;

void dab_scan_default_config(dab_scan_config_t *config);
int dab_scan_band_iii(const dab_scan_config_t *config, dab_scan_callback_t callback, void *user);

#ifdef __cplusplus
}
#endif

#endif
