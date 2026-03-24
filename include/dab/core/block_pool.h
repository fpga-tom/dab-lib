#ifndef DAB_CORE_BLOCK_POOL_H
#define DAB_CORE_BLOCK_POOL_H

#include <stdint.h>

#include "dab/core/block.h"
#include "dab/core/pc_queue.h"

typedef struct {
    dab_iq_block_t *blocks;
    void **free_storage;
    uint32_t block_count;
    dab_pc_queue_t free_blocks;
} dab_iq_block_pool_t;

int dab_iq_block_pool_init(dab_iq_block_pool_t *pool, dab_iq_block_t *blocks, void **storage, uint32_t block_count);
void dab_iq_block_pool_destroy(dab_iq_block_pool_t *pool);
int dab_iq_block_acquire(dab_iq_block_pool_t *pool, dab_iq_block_t **block);
int dab_iq_block_release(dab_iq_block_pool_t *pool, dab_iq_block_t *block);

#endif
