#ifndef DAB_RECEIVE_MEASUREMENT_INTERNAL_H
#define DAB_RECEIVE_MEASUREMENT_INTERNAL_H

#include <stdint.h>

#include "dab/core/ofdm.h"
#include "dab/core/prs_fft.h"
#include "dab/receive.h"

typedef struct {
    uint64_t frames_seen;
    int collecting;
    float spectrum_dbfs[DAB_RECEIVE_SPECTRUM_BIN_COUNT];
    float constellation_i[DAB_RECEIVE_CONSTELLATION_POINT_CAPACITY];
    float constellation_q[DAB_RECEIVE_CONSTELLATION_POINT_CAPACITY];
    uint32_t constellation_count;
    float cir_power_db[DAB_RECEIVE_CIR_BIN_COUNT];
    float channel_magnitude_db[DAB_RECEIVE_CHANNEL_RESPONSE_COUNT];
    float channel_phase_deg[DAB_RECEIVE_CHANNEL_RESPONSE_COUNT];
    double carrier_error_sum[DAB_RECEIVE_CARRIER_MER_COUNT];
    uint32_t carrier_symbol_count[DAB_RECEIVE_CARRIER_MER_COUNT];
    float carrier_mer_db[DAB_RECEIVE_CARRIER_MER_COUNT];
    float cir_i[DAB_RECEIVE_SPECTRUM_BIN_COUNT];
    float cir_q[DAB_RECEIVE_SPECTRUM_BIN_COUNT];
    int16_t null_scope_iq_interleaved[2u * DAB_RECEIVE_NULL_SCOPE_SAMPLE_CAPACITY];
    float null_scope_power_dbfs[DAB_RECEIVE_NULL_SCOPE_SAMPLE_CAPACITY];
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
    float null_scope_frame_power_dbfs[DAB_RECEIVE_NULL_SCOPE_FRAME_BIN_COUNT];
    uint32_t null_scope_frame_bin_count;
    float null_scope_frame_bin_samples;
    uint32_t null_scope_frame_sample_count;
} dab_receive_measurement_state_t;

void dab_receive_measurement_reset(dab_receive_measurement_state_t *state);
int dab_receive_measurement_begin(dab_receive_measurement_state_t *state,
                                  uint32_t decimation,
                                  const dab_prs_fft_t *fft,
                                  const float *fft_i,
                                  const float *fft_q,
                                  const dab_ofdm_t *ofdm);
int dab_receive_measurement_capture_null(dab_receive_measurement_state_t *state,
                                         const dab_sdr_block_view_t *samples,
                                         uint64_t prs_fft_start_sample,
                                         uint32_t sample_rate_hz,
                                         uint32_t null_samples,
                                         uint32_t guard_samples,
                                         uint32_t frame_samples);
void dab_receive_measurement_add_symbol(dab_receive_measurement_state_t *state,
                                        const float *physical_i,
                                        const float *physical_q,
                                        const float *constellation_i,
                                        const float *constellation_q,
                                        float common_phase_error_deg);
int dab_receive_measurement_finish(dab_receive_measurement_state_t *state,
                                   uint64_t sequence,
                                   uint64_t timestamp_ns,
                                   uint64_t frequency_hz,
                                   uint64_t frame_start_sample,
                                   uint32_t sample_rate_hz,
                                   dab_receive_measurement_frame_t *frame);

#endif
