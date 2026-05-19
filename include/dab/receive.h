#ifndef DAB_RECEIVE_H
#define DAB_RECEIVE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    DAB_RECEIVE_EVENT_STATUS = 1,
    DAB_RECEIVE_EVENT_AUDIO_FORMAT = 2,
    DAB_RECEIVE_EVENT_AUDIO_AU = 3,
    DAB_RECEIVE_EVENT_ERROR = 4
} dab_receive_event_type_t;

typedef struct {
    dab_receive_event_type_t type;
    const char *message;
    uint32_t service_id;
    uint64_t frequency_hz;
    uint32_t sample_rate;
    uint32_t channels;
    const uint8_t *asc;
    uint32_t asc_length;
    const uint8_t *audio_data;
    uint32_t audio_length;
    uint64_t generation;
} dab_receive_event_t;

typedef int (*dab_receive_callback_t)(const dab_receive_event_t *event, void *user);
typedef int (*dab_receive_cancel_callback_t)(void *user);

typedef struct {
    const char *driver;
    uint64_t frequency_hz;
    uint32_t service_id;
    int gain_db;
    int agc_enabled;
    int ppm_correction;
    int bias_tee_enabled;
    int use_real_device;
    int android_usb_fd;
    dab_receive_cancel_callback_t cancel;
    void *cancel_user;
} dab_receive_config_t;

void dab_receive_default_config(dab_receive_config_t *config);
int dab_receive_service(const dab_receive_config_t *config, dab_receive_callback_t callback, void *user);

#ifdef __cplusplus
}
#endif

#endif
