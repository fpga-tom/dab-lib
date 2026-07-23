#ifndef DAB_CORE_FIC_H
#define DAB_CORE_FIC_H

#include <stddef.h>
#include <stdint.h>

#include "dab/core/tframe.h"

#define DAB_FIC_BLOCKS_PER_FRAME 4u
#define DAB_FIC_FIBS_PER_BLOCK 3u
#define DAB_FIC_BYTES_PER_FIB 32u
#define DAB_FIC_BYTES_PER_BLOCK (DAB_FIC_FIBS_PER_BLOCK * DAB_FIC_BYTES_PER_FIB)
#define DAB_FIC_TOTAL_FIBS_PER_FRAME (DAB_FIC_BLOCKS_PER_FRAME * DAB_FIC_FIBS_PER_BLOCK)
#define DAB_FIC_DEPARTITIONED_SOFTBITS 2304u
#define DAB_FIC_UNPUNCTURED_BITS 3096u
#define DAB_FIC_TRELLIS_BITS 774u
#define DAB_FIC_MAX_SUBCHANNELS 64u
#define DAB_FIC_MAX_SERVICES 64u
#define DAB_FIC_MAX_COMPONENTS_PER_SERVICE 12u
#define DAB_FIC_MAX_LABELS 128u

typedef enum {
    DAB_FIC_FIG_TYPE_0 = 0,
    DAB_FIC_FIG_TYPE_1 = 1,
    DAB_FIC_FIG_TYPE_2 = 2,
    DAB_FIC_FIG_TYPE_3 = 3,
    DAB_FIC_FIG_TYPE_4 = 4,
    DAB_FIC_FIG_TYPE_6 = 6,
    DAB_FIC_FIG_TYPE_7 = 7
} dab_fic_fig_type_t;

typedef enum {
    DAB_FIC_PROTECTION_UEP = 0,
    DAB_FIC_PROTECTION_EEP = 1
} dab_fic_protection_kind_t;

typedef enum {
    DAB_FIC_LABEL_SCOPE_ENSEMBLE = 0,
    DAB_FIC_LABEL_SCOPE_SERVICE = 1,
    DAB_FIC_LABEL_SCOPE_COMPONENT = 2
} dab_fic_label_scope_t;

typedef struct {
    uint8_t type;
    uint8_t length;
    const uint8_t *payload;
} dab_fic_fig_t;

typedef struct {
    dab_fic_fig_type_t type;
    uint8_t ext;
    uint8_t cn;
    uint8_t oe;
    uint8_t pd;
    const uint8_t *data;
    uint8_t data_len;
} dab_fic_fig_view_t;

typedef struct {
    const uint8_t *data;
    size_t remaining;
} dab_fic_cursor_t;

typedef struct {
    const uint8_t *data;
    size_t bit_count;
    size_t bit_pos;
} dab_fic_bit_reader_t;

typedef struct {
    uint8_t valid;
    uint8_t subchid;
    uint16_t start_cu;
    uint16_t size_cu;
    dab_fic_protection_kind_t protection_kind;
    uint8_t table_switch;
    uint8_t protection_index;
    uint8_t protection_option;
    uint8_t protection_level;
} dab_fic_subchannel_t;

typedef struct {
    uint8_t used;
    uint8_t tmid;
    uint8_t sc_type;
    uint8_t primary;
    uint8_t ca_flag;
    uint8_t subchid;
    uint8_t has_subchid;
    uint16_t scid;
    uint16_t packet_addr;
    uint8_t has_packet_addr;
    uint8_t dg_flag;
    uint16_t caorg;
    uint8_t has_caorg;
} dab_fic_service_component_t;

typedef struct {
    uint8_t is_programme;
    uint32_t sid;
    uint8_t caid;
    uint8_t component_count;
    dab_fic_service_component_t components[DAB_FIC_MAX_COMPONENTS_PER_SERVICE];
} dab_fic_service_t;

typedef struct {
    uint8_t used;
    dab_fic_label_scope_t scope;
    uint16_t eid;
    uint8_t is_programme;
    uint32_t sid;
    uint8_t scids;
    uint8_t charset;
    char text[17];
    uint16_t short_label_mask;
} dab_fic_label_t;

typedef struct {
    uint16_t eid;
    dab_fic_subchannel_t subchannels[DAB_FIC_MAX_SUBCHANNELS];
    dab_fic_service_t services[DAB_FIC_MAX_SERVICES];
    uint16_t service_count;
    dab_fic_label_t labels[DAB_FIC_MAX_LABELS];
    uint16_t label_count;
} dab_fic_database_t;

typedef struct {
    dab_fic_database_t current;
    dab_fic_database_t next;
    uint64_t total_figs;
    uint64_t total_fig_parse_errors;
} dab_fic_repo_t;

typedef struct {
    uint32_t block_index;
    uint32_t polarity_variant;
    uint32_t pack_variant;
    uint32_t scramble_variant;
    uint32_t byte_variant;
    uint32_t valid_fibs;
} dab_fic_debug_block_t;

typedef struct dab_fic_decoder_workspace dab_fic_decoder_workspace_t;

typedef struct {
    uint32_t frames_seen;
    uint64_t last_frame_start_sample;
    uint32_t fic_blocks_decoded_last_frame;
    uint32_t valid_fibs_last_frame;
    uint32_t total_valid_fibs;
    dab_fic_debug_block_t block_info_last_frame[DAB_FIC_BLOCKS_PER_FRAME];
    dab_fic_repo_t repo;
    uint8_t decoded_fibs[DAB_FIC_TOTAL_FIBS_PER_FRAME][DAB_FIC_BYTES_PER_FIB];
    uint8_t fib_crc_ok[DAB_FIC_TOTAL_FIBS_PER_FRAME];
    dab_fic_decoder_workspace_t *decoder_workspace;
} dab_fic_state_t;

typedef struct {
    dab_fic_debug_block_t blocks[DAB_FIC_BLOCKS_PER_FRAME];
    uint32_t total_valid_fibs;
} dab_fic_debug_report_t;

typedef struct {
    dab_fic_debug_block_t info;
    int16_t depunctured[DAB_FIC_DEPARTITIONED_SOFTBITS];
    int16_t unpunctured[DAB_FIC_UNPUNCTURED_BITS];
    uint8_t decoded_bits[DAB_FIC_TRELLIS_BITS];
    uint8_t packed_lsb[DAB_FIC_BYTES_PER_BLOCK];
    uint8_t packed_msb[DAB_FIC_BYTES_PER_BLOCK];
    uint8_t decoded_block[DAB_FIC_BYTES_PER_BLOCK];
} dab_fic_debug_block_dump_t;

int dab_fic_init(dab_fic_state_t *state);
void dab_fic_reset(dab_fic_state_t *state);
void dab_fic_destroy(dab_fic_state_t *state);
int dab_fic_process_frame(dab_fic_state_t *state,
                          uint64_t frame_start_sample,
                          const int8_t *fic_softbits,
                          uint32_t fic_softbit_count);
int dab_fic_debug_analyze_frame(const int8_t *fic_softbits,
                                uint32_t fic_softbit_count,
                                dab_fic_debug_report_t *report);
int dab_fic_debug_decode_block(const int8_t *fic_softbits,
                               uint32_t fic_softbit_count,
                               uint32_t block_index,
                               dab_fic_debug_block_dump_t *dump);
void dab_fic_cursor_init(dab_fic_cursor_t *cursor, const uint8_t *data, size_t size);
int dab_fic_cursor_next_fig(dab_fic_cursor_t *cursor, dab_fic_fig_t *fig);
int dab_fic_fig_view_from_raw(const dab_fic_fig_t *fig, dab_fic_fig_view_t *view);
void dab_fic_bit_reader_init(dab_fic_bit_reader_t *reader, const uint8_t *data, size_t size);
int dab_fic_bit_reader_get(dab_fic_bit_reader_t *reader, uint8_t bit_count, uint32_t *value);
int dab_fic_parse_fib_payload(dab_fic_repo_t *repo, const uint8_t *fib_payload, size_t payload_size);

#endif
