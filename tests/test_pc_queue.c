#include "dab/core/pc_queue.h"
#include "dab/os/os_thread.h"

#include "test_support.h"

static int pop_thread_fn(void *arg) {
    dab_pc_queue_t *queue = (dab_pc_queue_t *)arg;
    void *item = 0;
    return dab_pc_queue_pop(queue, &item);
}

void test_pc_queue_basic(void) {
    void *storage[2] = {0};
    dab_pc_queue_t queue;
    int a = 11;
    int b = 22;
    void *item = 0;
    uint32_t count = 0u;
    uint32_t high_watermark = 0u;
    uint32_t capacity = 0u;

    TEST_ASSERT(dab_pc_queue_init(&queue, storage, 2u) == 0);
    TEST_ASSERT(dab_pc_queue_try_push(&queue, &a) == 0);
    TEST_ASSERT(dab_pc_queue_try_push(&queue, &b) == 0);
    dab_pc_queue_usage(&queue, &count, &high_watermark, &capacity);
    TEST_ASSERT(count == 2u);
    TEST_ASSERT(high_watermark == 2u);
    TEST_ASSERT(capacity == 2u);
    TEST_ASSERT(dab_pc_queue_try_push(&queue, &a) != 0);
    TEST_ASSERT(dab_pc_queue_try_pop(&queue, &item) == 0);
    TEST_ASSERT(item == &a);
    TEST_ASSERT(dab_pc_queue_try_pop(&queue, &item) == 0);
    TEST_ASSERT(item == &b);
    TEST_ASSERT(dab_pc_queue_try_pop(&queue, &item) != 0);
    dab_pc_queue_usage(&queue, &count, &high_watermark, &capacity);
    TEST_ASSERT(count == 0u);
    TEST_ASSERT(high_watermark == 2u);
    dab_pc_queue_destroy(&queue);
}

void test_pc_queue_stop_wakes_waiter(void) {
    void *storage[1] = {0};
    dab_pc_queue_t queue;
    dab_thread_t thread;
    int rc = 0;

    TEST_ASSERT(dab_pc_queue_init(&queue, storage, 1u) == 0);
    TEST_ASSERT(dab_thread_create(&thread, pop_thread_fn, &queue, "test-pop") == 0);
    dab_pc_queue_stop(&queue);
    TEST_ASSERT(dab_thread_join(&thread, &rc) == 0);
    TEST_ASSERT(rc != 0);
    dab_pc_queue_destroy(&queue);
}

void test_pc_queue_timed_pop_reports_timeout_and_reads_available_item(void) {
    void *storage[1] = {0};
    dab_pc_queue_t queue;
    int value = 42;
    void *item = 0;

    TEST_ASSERT(dab_pc_queue_init(&queue, storage, 1u) == 0);
    TEST_ASSERT(dab_pc_queue_pop_timeout(&queue, &item, 20u) == DAB_PC_QUEUE_TIMEOUT);
    TEST_ASSERT(item == 0);
    TEST_ASSERT(dab_pc_queue_try_push(&queue, &value) == DAB_PC_QUEUE_OK);
    TEST_ASSERT(dab_pc_queue_pop_timeout(&queue, &item, 20u) == DAB_PC_QUEUE_OK);
    TEST_ASSERT(item == &value);
    dab_pc_queue_destroy(&queue);
}
