#include <stdio.h>
#include <stdlib.h>

void test_pc_queue_basic(void);
void test_pc_queue_stop_wakes_waiter(void);
void test_block_pool_acquire_release(void);
void test_simd_scalar_fallback(void);
void test_simd_convert_and_dot(void);

int main(void) {
    test_pc_queue_basic();
    test_pc_queue_stop_wakes_waiter();
    test_block_pool_acquire_release();
    test_simd_scalar_fallback();
    test_simd_convert_and_dot();
    puts("all tests passed");
    return EXIT_SUCCESS;
}
