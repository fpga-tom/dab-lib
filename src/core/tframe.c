#include "dab/core/tframe.h"

#include <string.h>

int dab_tframe_init(dab_tframe_t *tframe, const dab_tframe_buffers_t *buffers) {
    if (tframe == 0 || buffers == 0 ||
        buffers->fic_softbits == 0 ||
        buffers->msc_softbits == 0 ||
        buffers->frame_softbits == 0) {
        return -1;
    }

    tframe->buffers = *buffers;
    dab_tframe_reset(tframe);
    return 0;
}

void dab_tframe_reset(dab_tframe_t *tframe) {
    if (tframe == 0) {
        return;
    }

    tframe->symbol_index = 0u;
    tframe->frame_start_sample = 0u;
}

int dab_tframe_push_symbol(dab_tframe_t *tframe,
                           uint64_t fft_start_sample,
                           const int8_t *soft_a,
                           const int8_t *soft_b,
                           dab_tframe_result_t *result) {
    uint32_t i;
    int8_t *dst;

    if (tframe == 0 || soft_a == 0 || soft_b == 0 || result == 0) {
        return -1;
    }

    if (tframe->symbol_index == 0u) {
        tframe->frame_start_sample = fft_start_sample;
    }

    dst = tframe->buffers.frame_softbits + (tframe->symbol_index * DAB_TFRAME_SOFTBITS_PER_SYMBOL);
    for (i = 0u; i < 1536u; ++i) {
        dst[i] = soft_a[i];
        dst[i + 1536u] = soft_b[i];
    }

    tframe->symbol_index++;

    if (tframe->symbol_index <= DAB_TFRAME_FIC_SYMBOLS) {
        memcpy(tframe->buffers.fic_softbits + ((tframe->symbol_index - 1u) * DAB_TFRAME_SOFTBITS_PER_SYMBOL),
               dst,
               DAB_TFRAME_SOFTBITS_PER_SYMBOL);
    } else {
        const uint32_t msc_symbol = tframe->symbol_index - DAB_TFRAME_FIC_SYMBOLS - 1u;
        memcpy(tframe->buffers.msc_softbits + (msc_symbol * DAB_TFRAME_SOFTBITS_PER_SYMBOL),
               dst,
               DAB_TFRAME_SOFTBITS_PER_SYMBOL);
    }

    result->symbol_index = tframe->symbol_index - 1u;
    result->frame_start_sample = tframe->frame_start_sample;
    result->frame_complete = 0;

    if (tframe->symbol_index >= DAB_TFRAME_TOTAL_SYMBOLS) {
        result->frame_complete = 1;
        dab_tframe_reset(tframe);
    }

    return 0;
}
