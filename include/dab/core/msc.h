#ifndef DAB_CORE_MSC_H
#define DAB_CORE_MSC_H

#include <stdint.h>

#include "dab/core/fic.h"
#include "dab/core/tframe.h"

enum {
    DAB_MSC_CIFS_PER_FRAME = 4u,
    DAB_MSC_CUS_PER_CIF = 864u,
    DAB_MSC_SOFTBITS_PER_CU = 64u,
    DAB_MSC_SOFTBITS_PER_CIF = DAB_MSC_CUS_PER_CIF * DAB_MSC_SOFTBITS_PER_CU,
    DAB_MSC_MAX_COMPONENT_MAPPINGS = DAB_FIC_MAX_SERVICES * DAB_FIC_MAX_COMPONENTS_PER_SERVICE,
    DAB_MSC_MAX_PUNCTURE_SEGMENTS = 5u,
    DAB_MSC_CONV_DEINTERLEAVER_DEPTH = 16u,
    DAB_MSC_MAX_CONV_BITS = (4u * 192u * 16u) + 32u,
    DAB_MSC_CONV_DEINTERLEAVER_GROUPS =
        (DAB_MSC_MAX_CONV_BITS + DAB_MSC_CONV_DEINTERLEAVER_DEPTH - 1u) /
        DAB_MSC_CONV_DEINTERLEAVER_DEPTH,
    DAB_MSC_MAX_LOGICAL_FRAME_BYTES_PER_CIF = 1152u,
    DAB_MSC_DABPLUS_COLUMNS_PER_SUPERFRAME = 120u,
    DAB_MSC_DABPLUS_COLUMNS_PER_RS_PAYLOAD = 110u,
    DAB_MSC_DABPLUS_RESYNC_COLUMNS = 24u,
    DAB_MSC_DABPLUS_MAX_AUS = 6u
};

typedef enum {
    DAB_MSC_PROFILE_UNKNOWN = 0,
    DAB_MSC_PROFILE_UEP,
    DAB_MSC_PROFILE_EEP_A,
    DAB_MSC_PROFILE_EEP_B
} dab_msc_protection_profile_t;

typedef struct {
    uint8_t valid;
    uint8_t subchid;
    uint16_t start_cu;
    uint16_t size_cu;
    uint16_t bitrate_kbps;
    dab_msc_protection_profile_t profile;
    uint8_t protection_level;
    uint8_t protection_index;
    uint8_t protection_option;
    uint8_t cif_count;
    uint32_t softbits_per_cif;
} dab_msc_subchannel_t;

typedef struct {
    uint8_t valid;
    uint8_t subchid;
    uint16_t start_cu;
    uint16_t size_cu;
    uint16_t bitrate_kbps;
    dab_msc_protection_profile_t profile;
    uint8_t protection_level;
    uint8_t cif_count;
    uint32_t logical_frame_bits;
    uint32_t coded_bits_per_cif;
    uint32_t coded_bits_per_frame;
    uint32_t cif_softbit_offset[DAB_MSC_CIFS_PER_FRAME];
    uint32_t cif_softbit_count[DAB_MSC_CIFS_PER_FRAME];
} dab_msc_subchannel_frame_t;

typedef struct {
    uint8_t code_index;
    uint8_t code_bits;
    uint32_t input_bits;
} dab_msc_puncture_segment_t;

typedef struct {
    uint8_t valid;
    uint8_t subchid;
    dab_msc_protection_profile_t profile;
    uint8_t protection_level;
    uint8_t padding_bits;
    uint16_t bitrate_kbps;
    uint16_t size_cu;
    uint32_t framebits;
    uint32_t conv_len;
    uint8_t segment_count;
    dab_msc_puncture_segment_t segments[DAB_MSC_MAX_PUNCTURE_SEGMENTS];
} dab_msc_decoder_config_t;

typedef struct {
    uint32_t conv_len;
    uint8_t write_index;
    int8_t regs[DAB_MSC_CONV_DEINTERLEAVER_GROUPS *
                DAB_MSC_CONV_DEINTERLEAVER_DEPTH *
                DAB_MSC_CONV_DEINTERLEAVER_DEPTH];
} dab_msc_deinterleaver_t;

typedef struct dab_msc_dabplus_stream_runtime dab_msc_dabplus_stream_runtime_t;
typedef struct dab_msc_decoder_workspace dab_msc_decoder_workspace_t;

typedef struct {
    uint8_t valid;
    uint8_t subchid;
    dab_msc_decoder_config_t config;
    uint32_t reset_count;
    uint8_t last_reconfig_reason;
    uint64_t ber_error_bits;
    uint64_t ber_compared_bits;
    uint64_t input_confidence_sum;
    uint64_t input_confidence_samples;
    uint64_t deinterleaver_cifs_seen;
    uint64_t decoded_cif_attempts;
    uint64_t decoded_cif_successes;
    uint64_t decoded_cif_failures;
    uint64_t decoded_output_bytes;
    dab_msc_deinterleaver_t *deinterleaver;
    dab_msc_decoder_workspace_t *workspace;
    dab_msc_dabplus_stream_runtime_t *dabplus;
} dab_msc_subchannel_runtime_t;

typedef struct {
    uint8_t used;
    uint8_t is_programme;
    uint8_t service_index;
    uint8_t component_index;
    uint8_t subchid;
    uint8_t primary;
    uint8_t tmid;
    uint8_t sc_type;
    uint16_t packet_addr;
    uint8_t has_packet_addr;
    uint8_t dg_flag;
    uint16_t caorg;
    uint8_t has_caorg;
    uint32_t sid;
} dab_msc_component_mapping_t;

typedef struct {
    uint8_t used;
    uint8_t service_index;
    uint8_t component_index;
    uint8_t subchid;
    uint8_t primary;
    uint8_t tmid;
    uint8_t sc_type;
    uint16_t packet_addr;
    uint8_t has_packet_addr;
    uint8_t dg_flag;
    uint16_t caorg;
    uint8_t has_caorg;
    uint32_t sid;
    uint32_t logical_frame_bits;
    uint32_t coded_bits_per_cif;
    uint32_t cif_softbit_offset[DAB_MSC_CIFS_PER_FRAME];
    uint32_t cif_softbit_count[DAB_MSC_CIFS_PER_FRAME];
} dab_msc_component_frame_t;

typedef struct {
    dab_msc_subchannel_t subchannels[DAB_FIC_MAX_SUBCHANNELS];
    uint16_t subchannel_count;
    dab_msc_component_mapping_t components[DAB_MSC_MAX_COMPONENT_MAPPINGS];
    uint16_t component_count;
} dab_msc_layout_t;

typedef struct {
    dab_msc_subchannel_frame_t subchannels[DAB_FIC_MAX_SUBCHANNELS];
    uint16_t subchannel_count;
    dab_msc_component_frame_t components[DAB_MSC_MAX_COMPONENT_MAPPINGS];
    uint16_t component_count;
} dab_msc_frame_view_t;

typedef struct {
    uint8_t valid;
    uint8_t subchid;
    uint8_t cif_valid[DAB_MSC_CIFS_PER_FRAME];
    uint16_t byte_count[DAB_MSC_CIFS_PER_FRAME];
    uint8_t data[DAB_MSC_CIFS_PER_FRAME][DAB_MSC_MAX_LOGICAL_FRAME_BYTES_PER_CIF];
} dab_msc_decoded_subchannel_t;

typedef struct {
    uint8_t used;
    uint8_t service_index;
    uint8_t component_index;
    uint8_t subchid;
    uint8_t primary;
    uint8_t tmid;
    uint8_t sc_type;
    uint16_t packet_addr;
    uint8_t has_packet_addr;
    uint8_t dg_flag;
    uint16_t caorg;
    uint8_t has_caorg;
    uint32_t sid;
} dab_msc_decoded_component_t;

typedef enum {
    DAB_MSC_COMPONENT_UNKNOWN = 0,
    DAB_MSC_COMPONENT_STREAM_AUDIO,
    DAB_MSC_COMPONENT_STREAM_DATA,
    DAB_MSC_COMPONENT_PACKET_MODE
} dab_msc_component_kind_t;

typedef struct {
    uint8_t valid;
    uint8_t service_index;
    uint8_t component_index;
    uint8_t subchid;
    uint8_t primary;
    uint8_t tmid;
    uint8_t sc_type;
    uint32_t sid;
    dab_msc_component_kind_t kind;
    uint16_t packet_addr;
    uint8_t has_packet_addr;
    uint8_t dg_flag;
    uint16_t caorg;
    uint8_t has_caorg;
    uint8_t cif_valid[DAB_MSC_CIFS_PER_FRAME];
    uint16_t byte_count[DAB_MSC_CIFS_PER_FRAME];
    const uint8_t *data[DAB_MSC_CIFS_PER_FRAME];
} dab_msc_component_payload_t;

typedef struct {
    uint8_t valid;
    uint8_t synchronized;
    uint64_t generation;
    uint8_t service_index;
    uint8_t component_index;
    uint8_t subchid;
    uint8_t primary;
    uint8_t tmid;
    uint8_t sc_type;
    uint32_t sid;
    uint16_t cif_bytes;
    uint32_t superframe_bytes;
    uint8_t dac_rate;
    uint8_t sbr_flag;
    uint8_t aac_channel_mode;
    uint8_t ps_flag;
    uint8_t mpeg_surround_config;
    uint8_t au_count;
    uint32_t au_start[DAB_MSC_DABPLUS_MAX_AUS];
    uint32_t au_length[DAB_MSC_DABPLUS_MAX_AUS];
    const uint8_t *superframe_data;
} dab_msc_dabplus_superframe_t;

typedef struct {
    uint8_t valid;
    uint8_t synchronized;
    uint8_t superframe_valid;
    uint8_t subchid;
    uint16_t cif_bytes;
    uint16_t columns_filled;
    uint64_t generation;
    uint32_t decode_attempts;
    uint32_t decode_failures;
    uint32_t rs_corrected_last_superframe;
    uint32_t rs_corrected_total;
    uint32_t au_crc_errors_last_superframe;
    uint32_t au_crc_errors_total;
    uint32_t au_crc_checks_total;
    uint8_t last_decode_ok;
    uint32_t runtime_reset_count;
    uint8_t last_reconfig_reason;
    uint64_t ber_error_bits;
    uint64_t ber_compared_bits;
} dab_msc_dabplus_debug_t;

typedef struct {
    uint32_t frames_seen;
    uint64_t last_frame_start_sample;
    int8_t first_softbit;
    int8_t last_softbit;
    dab_msc_layout_t layout;
    dab_msc_frame_view_t frame_view;
    dab_msc_subchannel_runtime_t runtimes[DAB_FIC_MAX_SUBCHANNELS];
    dab_msc_decoded_subchannel_t decoded_subchannels[DAB_FIC_MAX_SUBCHANNELS];
    dab_msc_decoded_component_t decoded_components[DAB_MSC_MAX_COMPONENT_MAPPINGS];
    uint16_t decoded_component_count;
    uint8_t service_filter_enabled;
    uint32_t service_filter_sid;
    uint8_t cif_counter_valid;
    uint16_t last_cif_count;
    uint32_t cif_discontinuity_count;
} dab_msc_state_t;

int dab_msc_init(dab_msc_state_t *state);
void dab_msc_reset(dab_msc_state_t *state);
void dab_msc_set_service_filter(dab_msc_state_t *state, uint32_t sid);
void dab_msc_clear_service_filter(dab_msc_state_t *state);
int dab_msc_build_layout(dab_msc_layout_t *layout, const dab_fic_database_t *db);
int dab_msc_resolve_subchannel(const dab_fic_subchannel_t *in,
                               dab_msc_subchannel_t *out);
int dab_msc_build_frame_view(dab_msc_frame_view_t *frame_view,
                             const dab_msc_layout_t *layout,
                             uint32_t msc_softbit_count);
int dab_msc_prepare_decoder_config(dab_msc_decoder_config_t *config, const dab_msc_subchannel_t *subchannel);
int dab_msc_sync_runtimes(dab_msc_subchannel_runtime_t *runtimes, const dab_msc_layout_t *layout);
void dab_msc_deinterleaver_reset(dab_msc_deinterleaver_t *state);
int dab_msc_deinterleaver_get_history(const dab_msc_deinterleaver_t *state,
                                      uint32_t bit_index,
                                      uint32_t age,
                                      int8_t *value);
int dab_msc_convolutional_deinterleave(dab_msc_deinterleaver_t *state,
                                       const int8_t *input,
                                       uint32_t input_count,
                                       int8_t *output,
                                       uint32_t output_count);
int dab_msc_deinterleave_subchannel_frame(const int8_t *msc_softbits,
                                          uint32_t msc_softbit_count,
                                          const dab_msc_subchannel_frame_t *frame,
                                          const dab_msc_decoder_config_t *config,
                                          dab_msc_deinterleaver_t *state,
                                          int8_t *output,
                                          uint32_t output_count);
int dab_msc_decode_deinterleaved_cif(const int8_t *deinterleaved_softbits,
                                     uint32_t deinterleaved_count,
                                     const dab_msc_decoder_config_t *config,
                                     uint8_t *output_bytes,
                                     uint32_t output_count);
int dab_msc_decode_deinterleaved_cif_debug(const int8_t *deinterleaved_softbits,
                                           uint32_t deinterleaved_count,
                                           const dab_msc_decoder_config_t *config,
                                           int16_t *unpunctured,
                                           uint32_t unpunctured_count,
                                           uint8_t *pre_descramble_bytes,
                                           uint32_t pre_descramble_count,
                                           uint8_t *post_descramble_bytes,
                                           uint32_t post_descramble_count,
                                           uint32_t *byte_count_out);
int dab_msc_decode_subchannel_cif(const int8_t *msc_softbits,
                                  uint32_t msc_softbit_count,
                                  const dab_msc_subchannel_frame_t *frame,
                                  const dab_msc_decoder_config_t *config,
                                  dab_msc_deinterleaver_t *state,
                                  uint32_t cif_index,
                                  uint8_t *output_bytes,
                                  uint32_t output_count);
const dab_msc_subchannel_t *dab_msc_find_subchannel(const dab_msc_layout_t *layout, uint8_t subchid);
const dab_msc_subchannel_frame_t *dab_msc_find_subchannel_frame(const dab_msc_frame_view_t *frame_view, uint8_t subchid);
dab_msc_component_kind_t dab_msc_component_kind(uint8_t tmid);
const dab_msc_decoded_component_t *dab_msc_find_decoded_component(const dab_msc_state_t *state,
                                                                  uint32_t sid,
                                                                  uint8_t component_index);
const dab_msc_decoded_component_t *dab_msc_find_primary_decoded_component(const dab_msc_state_t *state,
                                                                          uint32_t sid);
int dab_msc_get_component_payload(const dab_msc_state_t *state,
                                  const dab_msc_decoded_component_t *component,
                                  dab_msc_component_payload_t *payload);
int dab_msc_get_primary_component_payload(const dab_msc_state_t *state,
                                          uint32_t sid,
                                          dab_msc_component_payload_t *payload);
int dab_msc_ingest_component_payload(dab_msc_state_t *state,
                                     const dab_msc_decoded_component_t *component,
                                     const dab_msc_component_payload_t *payload);
int dab_msc_get_dabplus_superframe(const dab_msc_state_t *state,
                                   const dab_msc_decoded_component_t *component,
                                   dab_msc_dabplus_superframe_t *superframe);
int dab_msc_get_primary_dabplus_superframe(const dab_msc_state_t *state,
                                           uint32_t sid,
                                           dab_msc_dabplus_superframe_t *superframe);
int dab_msc_get_dabplus_debug(const dab_msc_state_t *state,
                              const dab_msc_decoded_component_t *component,
                              dab_msc_dabplus_debug_t *debug);
int dab_msc_get_primary_dabplus_debug(const dab_msc_state_t *state,
                                      uint32_t sid,
                                      dab_msc_dabplus_debug_t *debug);
int dab_msc_get_subchannel_softbits(const int8_t *msc_softbits,
                                    uint32_t msc_softbit_count,
                                    const dab_msc_subchannel_t *subchannel,
                                    uint32_t cif_index,
                                    const int8_t **slice,
                                    uint32_t *slice_count);
int dab_msc_process_frame(dab_msc_state_t *state,
                          uint64_t frame_start_sample,
                          const int8_t *msc_softbits,
                          uint32_t msc_softbit_count);
int dab_msc_process_frame_with_fic(dab_msc_state_t *state,
                                   uint64_t frame_start_sample,
                                   const int8_t *msc_softbits,
                                   uint32_t msc_softbit_count,
                                   const dab_fic_database_t *db);

#endif
