#ifndef DAB_CORE_FIC_H
#define DAB_CORE_FIC_H

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

typedef struct {
    uint32_t block_index;
    uint32_t polarity_variant;
    uint32_t pack_variant;
    uint32_t scramble_variant;
    uint32_t byte_variant;
    uint32_t valid_fibs;
} dab_fic_debug_block_t;

typedef struct {
    uint32_t frames_seen;
    uint64_t last_frame_start_sample;
    uint32_t fic_blocks_decoded_last_frame;
    uint32_t valid_fibs_last_frame;
    uint32_t total_valid_fibs;
    dab_fic_debug_block_t block_info_last_frame[DAB_FIC_BLOCKS_PER_FRAME];
    uint8_t decoded_fibs[DAB_FIC_TOTAL_FIBS_PER_FRAME][DAB_FIC_BYTES_PER_FIB];
    uint8_t fib_crc_ok[DAB_FIC_TOTAL_FIBS_PER_FRAME];
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

#endif
