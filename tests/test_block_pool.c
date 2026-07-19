#include "dab/core/block_pool.h"

#include <stdint.h>

#include "test_support.h"

void test_block_pool_acquire_release(void) {
    dab_iq_block_t blocks[2];
    void *storage[2] = {0};
    dab_iq_block_pool_t pool;
    dab_iq_block_t *a = 0;
    dab_iq_block_t *b = 0;

    TEST_ASSERT(dab_iq_block_pool_init(&pool, blocks, storage, 2u) == 0);
    TEST_ASSERT(dab_iq_block_acquire(&pool, &a) == 0);
    TEST_ASSERT(dab_iq_block_acquire(&pool, &b) == 0);
    TEST_ASSERT(a != 0);
    TEST_ASSERT(b != 0);
    TEST_ASSERT(a != b);
    TEST_ASSERT(dab_iq_block_release(&pool, a) == 0);
    TEST_ASSERT(dab_iq_block_release(&pool, b) == 0);
    dab_pc_queue_stop(&pool.free_blocks);
    dab_iq_block_pool_destroy(&pool);
}

void test_block_storage_is_compact_and_aligned(void) {
    dab_iq_block_storage_t storage;
    const size_t payload_bytes = 4u * sizeof(dab_iq_block_t);
    const size_t queue_bytes = 7u * sizeof(void *);

    TEST_ASSERT(dab_iq_block_storage_init(&storage, 4u, 3u) == 0);
    TEST_ASSERT(storage.allocation != 0);
    TEST_ASSERT(storage.blocks != 0);
    TEST_ASSERT(storage.free_storage != 0);
    TEST_ASSERT(storage.filled_storage == storage.free_storage + 4u);
    TEST_ASSERT(storage.block_count == 4u);
    TEST_ASSERT(storage.queue_capacity == 3u);
    TEST_ASSERT(((uintptr_t)storage.blocks & (DAB_IQ_BLOCK_ALIGNMENT - 1u)) == 0u);
    TEST_ASSERT(storage.allocation_bytes ==
                payload_bytes + queue_bytes + DAB_IQ_BLOCK_ALIGNMENT - 1u);

    dab_iq_block_storage_destroy(&storage);
    TEST_ASSERT(storage.allocation == 0);
    TEST_ASSERT(storage.blocks == 0);
}
