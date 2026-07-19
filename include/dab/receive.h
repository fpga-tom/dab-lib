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
    uint64_t sequence;
    uint64_t timestamp_ns;
    uint64_t frequency_hz;
    uint64_t frame_start_sample;
    uint64_t blocks_received;
    uint32_t sample_rate_hz;
    uint32_t dropped_blocks;

    int locked;
    uint32_t good_frames;
    uint32_t bad_frames;

    int adc_level_valid;
    float adc_level_dbfs;
    int prs_valid;
    float prs_score;
    int cn_valid;
    float cn_db;
    int snr_valid;
    float snr_db;
    int cfo_valid;
    float cfo_hz;
    float fine_cfo_hz;
    int32_t carrier_offset_bins;
    int timing_valid;
    int32_t timing_error_samples;

    uint32_t fic_frames_seen;
    uint32_t fic_blocks_decoded_last_frame;
    uint32_t fic_valid_fibs_last_frame;
    uint32_t fic_total_valid_fibs;

    uint32_t msc_frames_seen;
    int msc_audio_valid;
    int msc_synchronized;
    int msc_superframe_valid;
    uint64_t msc_superframe_generation;
    uint32_t msc_decode_attempts;
    uint32_t msc_decode_failures;
    uint32_t msc_rs_corrected_last_superframe;
    uint32_t msc_rs_corrected_total;
    int msc_last_decode_ok;
    uint32_t msc_runtime_reset_count;
    uint8_t msc_last_reconfig_reason;
} dab_receive_snapshot_t;

typedef int (*dab_receive_snapshot_callback_t)(const dab_receive_snapshot_t *snapshot, void *user);

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
    dab_receive_snapshot_callback_t snapshot;
    void *snapshot_user;
} dab_receive_config_t;

void dab_receive_default_config(dab_receive_config_t *config);
int dab_receive_service(const dab_receive_config_t *config, dab_receive_callback_t callback, void *user);

#ifdef __cplusplus
}
#endif

#endif
