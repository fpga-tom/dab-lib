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
    DAB_RECEIVE_EVENT_ERROR = 4,
    DAB_RECEIVE_EVENT_STREAM_STALLED = 5,
    DAB_RECEIVE_EVENT_DECODER_STALLED = 6
} dab_receive_event_type_t;

enum {
    DAB_RECEIVE_RESULT_CANCELLED = -2,
    DAB_RECEIVE_RESULT_STREAM_STALLED = -3,
    DAB_RECEIVE_RESULT_DECODER_STALLED = -4
};

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

enum {
    DAB_RECEIVE_ADC_HISTOGRAM_BIN_COUNT = 64u
};

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
    float adc_peak_dbfs;
    float adc_crest_factor_db;
    uint32_t adc_window_sample_count;
    uint32_t adc_clipped_sample_count;
    float adc_clipping_percent;
    float adc_i_dc;
    float adc_q_dc;
    int adc_iq_quality_valid;
    float adc_iq_amplitude_imbalance_db;
    float adc_iq_phase_imbalance_deg;
    float adc_iq_correlation;
    uint32_t adc_i_histogram[DAB_RECEIVE_ADC_HISTOGRAM_BIN_COUNT];
    uint32_t adc_q_histogram[DAB_RECEIVE_ADC_HISTOGRAM_BIN_COUNT];
    int calibrated_power_valid;
    float calibrated_power_dbm;

    int device_gain_valid;
    int device_gain_db;
    int lna_state_valid;
    uint8_t lna_state;
    int if_gain_reduction_valid;
    int if_gain_reduction_db;
    int agc_enabled;
    int device_temperature_valid;
    float device_temperature_c;
    uint32_t input_queue_depth;
    uint32_t input_queue_high_watermark;
    uint32_t input_queue_capacity;
    uint64_t input_block_interval_ns;
    uint64_t processing_latency_ns;
    uint32_t processing_late_count;
    int hardware_overruns_valid;
    uint32_t hardware_overruns;

    int prs_valid;
    float prs_score;
    int cn_valid;
    float cn_db;
    int snr_valid;
    float snr_db;
    int cfo_valid;
    float cfo_hz;
    float fine_cfo_hz;
    float residual_cfo_hz;
    int32_t carrier_offset_bins;
    int timing_valid;
    int32_t timing_error_samples;
    int fft_timing_valid;
    float fft_timing_offset_samples;
    int null_confidence_valid;
    float null_confidence;
    int cp_correlation_valid;
    float cp_correlation_peak;
    int sample_clock_error_valid;
    float sample_clock_error_ppm;
    uint32_t sample_clock_frame_count;
    int sync_acquisition_time_valid;
    float sync_acquisition_time_ms;
    uint64_t sync_lock_duration_ns;
    uint32_t sync_loss_count;
    uint32_t sync_reacquisition_count;

    uint32_t fic_frames_seen;
    uint32_t fic_blocks_decoded_last_frame;
    uint32_t fic_valid_fibs_last_frame;
    uint32_t fic_total_valid_fibs;
    uint64_t fic_total_figs;
    uint64_t fic_total_fig_parse_errors;

    uint32_t msc_frames_seen;
    int msc_ber_valid;
    float msc_ber;
    uint64_t msc_ber_error_bits;
    uint64_t msc_ber_compared_bits;
    int msc_audio_valid;
    int msc_synchronized;
    int msc_superframe_valid;
    uint64_t msc_superframe_generation;
    uint32_t msc_decode_attempts;
    uint32_t msc_decode_failures;
    uint32_t msc_rs_corrected_last_superframe;
    uint32_t msc_rs_corrected_total;
    uint32_t msc_au_crc_errors_last_superframe;
    uint32_t msc_au_crc_errors_total;
    uint32_t msc_au_crc_checks_total;
    int msc_last_decode_ok;
    uint32_t msc_runtime_reset_count;
    uint8_t msc_last_reconfig_reason;
} dab_receive_snapshot_t;

typedef int (*dab_receive_snapshot_callback_t)(const dab_receive_snapshot_t *snapshot, void *user);

enum {
    DAB_RECEIVE_SPECTRUM_BIN_COUNT = 2048u,
    DAB_RECEIVE_CONSTELLATION_POINT_CAPACITY = 512u,
    DAB_RECEIVE_CIR_BIN_COUNT = 512u,
    DAB_RECEIVE_CHANNEL_RESPONSE_COUNT = 1536u,
    DAB_RECEIVE_CARRIER_MER_COUNT = 1536u,
    DAB_RECEIVE_NULL_SCOPE_SAMPLE_CAPACITY = 32768u,
    DAB_RECEIVE_NULL_SCOPE_FRAME_BIN_COUNT = 2048u
};

/*
 * The array views below are owned by the receiver and remain valid only for
 * the duration of the measurement callback. Consumers that cross threads
 * must copy the arrays before returning.
 *
 * Spectrum bins are FFT-shifted from -sample_rate/2 to +sample_rate/2.
 * Constellation I/Q values have common phase removed and are normalized to
 * unit average complex power; ideal QPSK components are +/-1/sqrt(2).
 * CIR bins contain Hann-windowed channel impulse power, normalized to the
 * strongest exported tap (0 dB), with one bin per input sample period.
 * Channel response entries contain the PRS-derived channel magnitude,
 * normalized to mean active-carrier power (0 dB), and wrapped phase in
 * degrees. Entries use the same physical-carrier mapping as carrier MER.
 * Carrier MER entries 0..767 map to carriers -768..-1 and entries 768..1535
 * map to carriers +1..+768.
 * Null-scope power is calculated directly from receiver IQ as
 * 10*log10((I^2+Q^2)/32768^2).  The focused IQ/power arrays retain one entry
 * per input sample around the aligned Mode-I null.  The frame array contains
 * mean linear power reduced to fixed bins across the complete 96 ms frame.
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

    const float *channel_magnitude_db;
    const float *channel_phase_deg;
    uint32_t channel_response_count;

    const float *carrier_mer_db;
    uint32_t carrier_mer_count;
    int32_t first_carrier;
    uint32_t negative_carrier_count;
    float carrier_spacing_hz;

    const int16_t *null_scope_iq_interleaved;
    const float *null_scope_power_dbfs;
    uint32_t null_scope_sample_count;
    int32_t null_scope_start_offset_samples;
    uint32_t null_scope_start_index;
    uint32_t null_scope_end_index;
    uint32_t null_scope_duration_samples;
    float null_scope_sample_period_ns;
    float null_scope_threshold_dbfs;
    float null_scope_depth_db;
    float null_scope_confidence;
    float null_scope_energy_dbfs;

    const float *null_scope_frame_power_dbfs;
    uint32_t null_scope_frame_bin_count;
    float null_scope_frame_bin_samples;
    uint32_t null_scope_frame_sample_count;
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
    uint16_t subchannel_bitrate_kbps;
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
    /* Zero disables the SDR input watchdog. */
    uint32_t stream_watchdog_timeout_ms;
    /* Zero disables the locked DAB decoder-progress watchdog. */
    uint32_t decoder_watchdog_timeout_ms;
    dab_receive_catalog_callback_t catalog;
    void *catalog_user;
} dab_receive_config_t;

void dab_receive_default_config(dab_receive_config_t *config);
int dab_receive_service(const dab_receive_config_t *config, dab_receive_callback_t callback, void *user);

#ifdef __cplusplus
}
#endif

#endif
