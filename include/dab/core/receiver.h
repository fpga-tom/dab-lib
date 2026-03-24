#ifndef DAB_CORE_RECEIVER_H
#define DAB_CORE_RECEIVER_H

#include "dab/config.h"
#include "dab/core/block_pool.h"
#include "dab/core/pc_queue.h"

typedef struct {
    dab_stream_config_t stream;
    uint32_t queue_capacity;
    uint32_t block_count;
} dab_receiver_config_t;

typedef struct {
    dab_receiver_config_t config;
    dab_iq_block_pool_t iq_pool;
    dab_pc_queue_t filled_iq;
    void **filled_storage;
    dab_iq_block_t *blocks;
    void **free_storage;
} dab_receiver_t;

int dab_receiver_init(dab_receiver_t *receiver, const dab_receiver_config_t *config);
void dab_receiver_shutdown(dab_receiver_t *receiver);

#endif
