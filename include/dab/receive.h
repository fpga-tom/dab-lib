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

enum {
    DAB_RECEIVE_SPECTRUM_BIN_COUNT = 2048u,
    DAB_RECEIVE_CONSTELLATION_POINT_CAPACITY = 512u,
    DAB_RECEIVE_CIR_BIN_COUNT = 512u,
    DAB_RECEIVE_CARRIER_MER_COUNT = 1536u
};

/*
 * The array views below are owned by the receiver and remain valid only for
 * the duration of the measurement callback. Consumers that cross threads
 * must copy the arrays before returning.
 *
 * Spectrum bins are FFT-shifted from -sample_rate/2 to +sample_rate/2.
 * CIR bins contain Hann-windowed channel impulse power, normalized to the
 * strongest exported tap (0 dB), with one bin per input sample period.
 * Carrier MER entries 0..767 map to carriers -768..-1 and entries 768..1535
 * map to carriers +1..+768.
 */
typedef struct {
    uint64_t sequence;
    uint64_t timestamp_ns;
    uint64_t frequency_hz;
    uint64_t frame_start_sample;
    uint32_t sample_rate_hz;

    const float *spectrum_dbfs;
    uint32_t spectrum_bin_count;
    float spectrum_start_hz;
    float spectrum_bin_hz;

    const float *constellation_i;
    const float *constellation_q;
    uint32_t constellation_point_count;

    const float *cir_power_db;
    uint32_t cir_bin_count;
    float cir_sample_period_ns;

    const float *carrier_mer_db;
    uint32_t carrier_mer_count;
    int32_t first_carrier;
    uint32_t negative_carrier_count;
    float carrier_spacing_hz;
} dab_receive_measurement_frame_t;

typedef int (*dab_receive_measurement_callback_t)(
    const dab_receive_measurement_frame_t *frame,
    void *user);

enum {
    DAB_RECEIVE_CATALOG_LABEL_CAPACITY = 17u,
    DAB_RECEIVE_CATALOG_MAX_SERVICES = 64u,
    DAB_RECEIVE_CATALOG_MAX_COMPONENTS_PER_SERVICE = 12u
};

typedef enum {
    DAB_RECEIVE_COMPONENT_UNKNOWN = 0,
    DAB_RECEIVE_COMPONENT_STREAM_AUDIO = 1,
    DAB_RECEIVE_COMPONENT_STREAM_DATA = 2,
    DAB_RECEIVE_COMPONENT_PACKET_MODE = 3
} dab_receive_component_kind_t;

typedef struct {
    uint8_t component_index;
    dab_receive_component_kind_t kind;
    uint8_t transport_mode_id;
    uint8_t service_component_type;
    uint8_t primary;
    uint8_t conditional_access;

    uint8_t has_subchannel;
    uint8_t subchannel_id;
    uint8_t subchannel_valid;
    uint16_t subchannel_start_cu;
    uint16_t subchannel_size_cu;
    uint8_t protection_kind;
    uint8_t protection_table_switch;
    uint8_t protection_index;
    uint8_t protection_option;
    uint8_t protection_level;

    uint16_t service_component_id;
    uint8_t has_packet_address;
    uint16_t packet_address;
    uint8_t data_group;
} dab_receive_component_info_t;

typedef struct {
    uint32_t service_id;
    uint8_t is_programme;
    uint8_t conditional_access_id;
    uint8_t label_charset;
    char label[DAB_RECEIVE_CATALOG_LABEL_CAPACITY];
    uint32_t component_count;
    dab_receive_component_info_t
        components[DAB_RECEIVE_CATALOG_MAX_COMPONENTS_PER_SERVICE];
} dab_receive_service_info_t;

/*
 * The catalog is a complete owned copy of the currently decoded FIC database.
 * It remains valid only for the duration of the callback, but may be copied as
 * a value by consumers that cross threads. A callback is made only when its
 * ensemble, services, labels, or components change.
 */
typedef struct {
    uint64_t sequence;
    uint64_t timestamp_ns;
    uint64_t frequency_hz;
    uint16_t ensemble_id;
    uint8_t ensemble_label_charset;
    char ensemble_label[DAB_RECEIVE_CATALOG_LABEL_CAPACITY];
    uint32_t service_count;
    dab_receive_service_info_t services[DAB_RECEIVE_CATALOG_MAX_SERVICES];
} dab_receive_catalog_t;

typedef int (*dab_receive_catalog_callback_t)(
    const dab_receive_catalog_t *catalog,
    void *user);

typedef struct {
    const char *driver;
    uint64_t frequency_hz;
    uint32_t service_id; /* Zero selects the first decoded audio service. */
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
    dab_receive_measurement_callback_t measurement;
    void *measurement_user;
    uint32_t measurement_decimation;
    dab_receive_catalog_callback_t catalog;
    void *catalog_user;
} dab_receive_config_t;

void dab_receive_default_config(dab_receive_config_t *config);
int dab_receive_service(const dab_receive_config_t *config, dab_receive_callback_t callback, void *user);

#ifdef __cplusplus
}
#endif

#endif
