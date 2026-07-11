#include "dab/core/block_pool.h"

#include <stdint.h>
#include <stdlib.h>

int dab_iq_block_storage_init(dab_iq_block_storage_t *storage,
                              uint32_t block_count,
                              uint32_t queue_capacity) {
    const size_t alignment = DAB_IQ_BLOCK_ALIGNMENT;
    size_t block_bytes;
    size_t pointer_count;
    size_t pointer_bytes;
    size_t allocation_bytes;
    uintptr_t aligned_address;
    void *allocation;

    if (storage == 0 || block_count == 0u || queue_capacity == 0u ||
        (alignment & (alignment - 1u)) != 0u ||
        (size_t)block_count > SIZE_MAX / sizeof(dab_iq_block_t) ||
        (size_t)queue_capacity > SIZE_MAX - (size_t)block_count) {
        return -1;
    }

    *storage = (dab_iq_block_storage_t){0};
    block_bytes = (size_t)block_count * sizeof(dab_iq_block_t);
    pointer_count = (size_t)block_count + (size_t)queue_capacity;
    if (pointer_count > SIZE_MAX / sizeof(void *)) {
        return -1;
    }
    pointer_bytes = pointer_count * sizeof(void *);
    if (block_bytes > SIZE_MAX - pointer_bytes ||
        block_bytes + pointer_bytes > SIZE_MAX - (alignment - 1u)) {
        return -1;
    }
    allocation_bytes = block_bytes + pointer_bytes + (alignment - 1u);
    allocation = malloc(allocation_bytes);
    if (allocation == 0) {
        return -1;
    }

    aligned_address = ((uintptr_t)allocation + alignment - 1u) & ~(uintptr_t)(alignment - 1u);
    storage->allocation = allocation;
    storage->allocation_bytes = allocation_bytes;
    storage->blocks = (dab_iq_block_t *)aligned_address;
    storage->free_storage = (void **)((unsigned char *)storage->blocks + block_bytes);
    storage->filled_storage = storage->free_storage + block_count;
    storage->block_count = block_count;
    storage->queue_capacity = queue_capacity;
    return 0;
}

void dab_iq_block_storage_destroy(dab_iq_block_storage_t *storage) {
    if (storage == 0) {
        return;
    }
    free(storage->allocation);
    *storage = (dab_iq_block_storage_t){0};
}

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
