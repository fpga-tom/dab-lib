#include "dab/core/block_pool.h"

int dab_iq_block_pool_init(dab_iq_block_pool_t *pool, dab_iq_block_t *blocks, void **storage, uint32_t block_count) {
    uint32_t i;

    if (pool == 0 || blocks == 0 || storage == 0 || block_count == 0u) {
        return -1;
    }

    pool->blocks = blocks;
    pool->free_storage = storage;
    pool->block_count = block_count;

    if (dab_pc_queue_init(&pool->free_blocks, storage, block_count) != 0) {
        return -1;
    }

    for (i = 0u; i < block_count; ++i) {
        blocks[i].sequence = 0u;
        blocks[i].timestamp_ns = 0u;
        blocks[i].valid_samples = 0u;
        blocks[i].flags = 0u;
        if (dab_pc_queue_push(&pool->free_blocks, &blocks[i]) != 0) {
            dab_iq_block_pool_destroy(pool);
            return -1;
        }
    }

    return 0;
}

void dab_iq_block_pool_destroy(dab_iq_block_pool_t *pool) {
    if (pool == 0) {
        return;
    }
    dab_pc_queue_destroy(&pool->free_blocks);
}

int dab_iq_block_acquire(dab_iq_block_pool_t *pool, dab_iq_block_t **block) {
    return dab_pc_queue_pop(&pool->free_blocks, (void **)block);
}

int dab_iq_block_release(dab_iq_block_pool_t *pool, dab_iq_block_t *block) {
    return dab_pc_queue_push(&pool->free_blocks, block);
}
