#ifndef DAB_CORE_TFRAME_H
#define DAB_CORE_TFRAME_H

#include <stdint.h>

enum {
    DAB_TFRAME_FIC_SYMBOLS = 3u,
    DAB_TFRAME_MSC_SYMBOLS = 72u,
    DAB_TFRAME_TOTAL_SYMBOLS = 75u,
    DAB_TFRAME_SOFTBITS_PER_SYMBOL = 3072u,
    DAB_TFRAME_FIC_SOFTBITS = DAB_TFRAME_FIC_SYMBOLS * DAB_TFRAME_SOFTBITS_PER_SYMBOL,
    DAB_TFRAME_MSC_SOFTBITS = DAB_TFRAME_MSC_SYMBOLS * DAB_TFRAME_SOFTBITS_PER_SYMBOL,
    DAB_TFRAME_TOTAL_SOFTBITS = DAB_TFRAME_TOTAL_SYMBOLS * DAB_TFRAME_SOFTBITS_PER_SYMBOL
};

typedef struct {
    int8_t *fic_softbits;
    int8_t *msc_softbits;
    int8_t *frame_softbits;
} dab_tframe_buffers_t;

typedef struct {
    uint32_t symbol_index;
    uint64_t frame_start_sample;
    int frame_complete;
} dab_tframe_result_t;

typedef struct {
    uint32_t symbol_index;
    uint64_t frame_start_sample;
    dab_tframe_buffers_t buffers;
} dab_tframe_t;

int dab_tframe_init(dab_tframe_t *tframe, const dab_tframe_buffers_t *buffers);
void dab_tframe_reset(dab_tframe_t *tframe);
int dab_tframe_push_symbol(dab_tframe_t *tframe,
                           uint64_t fft_start_sample,
                           const int8_t *soft_a,
                           const int8_t *soft_b,
                           dab_tframe_result_t *result);

#endif
