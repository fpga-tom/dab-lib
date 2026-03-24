#include "dab/core/block_pool.h"

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
