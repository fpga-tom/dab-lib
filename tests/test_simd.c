#include "dab/simd/simd.h"

#include "test_support.h"

void test_simd_scalar_fallback(void) {
    const dab_simd_ops_t *ops = dab_simd_get_ops();
    TEST_ASSERT(ops != 0);
    TEST_ASSERT(ops->complex_i16_to_f32 != 0);
    TEST_ASSERT(ops->dot_f32 != 0);
}

void test_simd_convert_and_dot(void) {
    const dab_simd_ops_t *ops = dab_simd_get_ops();
    const int16_t iq[] = {1, -2, 3, -4, 5, -6};
    float i[3] = {0};
    float q[3] = {0};
    const float a[] = {1.0f, 2.0f, 3.0f};
    const float b[] = {4.0f, 5.0f, 6.0f};

    ops->complex_i16_to_f32(i, q, iq, 3u);

    TEST_ASSERT(i[0] == 1.0f);
    TEST_ASSERT(q[0] == -2.0f);
    TEST_ASSERT(i[2] == 5.0f);
    TEST_ASSERT(q[2] == -6.0f);
    TEST_ASSERT(ops->dot_f32(a, b, 3u) == 32.0f);
}
