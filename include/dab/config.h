#ifndef DAB_CONFIG_H
#define DAB_CONFIG_H

#include <stdint.h>

#define DAB_IQ_BLOCK_SAMPLES 16384u
#define DAB_IQ_BLOCK_ALIGNMENT 32u
#define DAB_DEFAULT_QUEUE_CAPACITY 32u
#ifndef DAB_CAPTURE_BLOCK_COUNT
#define DAB_CAPTURE_BLOCK_COUNT 32u
#endif

typedef struct {
    uint32_t sample_rate;
    uint64_t center_freq_hz;
    uint32_t bandwidth_hz;
    int gain_db;
} dab_stream_config_t;

#endif
