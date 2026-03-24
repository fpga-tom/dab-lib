#ifndef DAB_CORE_BLOCK_H
#define DAB_CORE_BLOCK_H

#include "dab/config.h"
#include "dab/types.h"

typedef struct DAB_ALIGN(DAB_IQ_BLOCK_ALIGNMENT) {
    uint64_t sequence;
    uint64_t timestamp_ns;
    uint32_t valid_samples;
    uint32_t flags;
    int16_t iq[2u * DAB_IQ_BLOCK_SAMPLES];
} dab_iq_block_t;

#endif
