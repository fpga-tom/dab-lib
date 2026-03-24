#ifndef DAB_CORE_FRONTEND_H
#define DAB_CORE_FRONTEND_H

#include <stdint.h>

#include "dab/config.h"
#include "dab/hal/sdr_device.h"

typedef struct {
    float dc_alpha;
} dab_frontend_config_t;

typedef struct {
    uint64_t sample_index;
    uint64_t timestamp_ns;
    uint32_t sample_count;
    float *i_data;
    float *q_data;
} dab_frontend_block_t;

typedef struct {
    dab_frontend_config_t config;
    float dc_i;
    float dc_q;
    uint64_t processed_samples;
} dab_frontend_t;

void dab_frontend_default_config(dab_frontend_config_t *config);
int dab_frontend_init(dab_frontend_t *frontend, const dab_frontend_config_t *config);
void dab_frontend_reset(dab_frontend_t *frontend);
int dab_frontend_process(dab_frontend_t *frontend,
                         const dab_sdr_block_view_t *src,
                         dab_frontend_block_t *dst);

#endif
