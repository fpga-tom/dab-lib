#include "dab/simd/simd.h"

#include "test_support.h"

void test_simd_scalar_fallback(void) {
    const dab_simd_ops_t *ops = dab_simd_get_ops();
    TEST_ASSERT(ops != 0);
    TEST_ASSERT(ops->complex_i16_to_f32 != 0);
    TEST_ASSERT(ops->power_i16_interleaved_f32 != 0);
    TEST_ASSERT(ops->dot_f32 != 0);
    TEST_ASSERT(ops->mul_f32 != 0);
    TEST_ASSERT(ops->complex_abs2_f32 != 0);
}

void test_simd_convert_and_dot(void) {
    const dab_simd_ops_t *ops = dab_simd_get_ops();
    const int16_t iq[] = {1, -2, 3, -4, 5, -6};
    float i[3] = {0};
    float q[3] = {0};
    float p[3] = {0};
    float product[3] = {0};
    float abs2[3] = {0};
    const float a[] = {1.0f, 2.0f, 3.0f};
    const float b[] = {4.0f, 5.0f, 6.0f};

    ops->complex_i16_to_f32(i, q, iq, 3u);
    ops->power_i16_interleaved_f32(p, iq, 3u);
    ops->mul_f32(product, a, b, 3u);
    ops->complex_abs2_f32(abs2, i, q, 3u);

    TEST_ASSERT(i[0] == 1.0f);
    TEST_ASSERT(q[0] == -2.0f);
    TEST_ASSERT(i[2] == 5.0f);
    TEST_ASSERT(q[2] == -6.0f);
    TEST_ASSERT(p[0] == 5.0f);
    TEST_ASSERT(p[1] == 25.0f);
    TEST_ASSERT(p[2] == 61.0f);
    TEST_ASSERT(product[0] == 4.0f);
    TEST_ASSERT(product[1] == 10.0f);
    TEST_ASSERT(product[2] == 18.0f);
    TEST_ASSERT(abs2[0] == 5.0f);
    TEST_ASSERT(abs2[1] == 25.0f);
    TEST_ASSERT(abs2[2] == 61.0f);
    TEST_ASSERT(ops->dot_f32(a, b, 3u) == 32.0f);
}

void test_simd_complex_convert_vector_and_tail(void) {
    enum { COUNT = 25 };
    const dab_simd_ops_t *ops = dab_simd_get_ops();
    int16_t storage[(2u * COUNT) + 1u];
    int16_t *iq = storage + 1u;
    float i_storage[COUNT + 2u];
    float q_storage[COUNT + 2u];
    float *i_out = i_storage + 1u;
    float *q_out = q_storage + 1u;
    uint32_t index;

    i_storage[0] = i_storage[COUNT + 1u] = 123456.0f;
    q_storage[0] = q_storage[COUNT + 1u] = -123456.0f;
    for (index = 0u; index < COUNT; ++index) {
        iq[2u * index] = (int16_t)((index * 5003u) - 32768u);
        iq[(2u * index) + 1u] = (int16_t)(32767 - (int32_t)(index * 4099u));
    }

    ops->complex_i16_to_f32(i_out, q_out, iq, COUNT);

    for (index = 0u; index < COUNT; ++index) {
        TEST_ASSERT(i_out[index] == (float)iq[2u * index]);
        TEST_ASSERT(q_out[index] == (float)iq[(2u * index) + 1u]);
    }
    TEST_ASSERT(i_storage[0] == 123456.0f);
    TEST_ASSERT(i_storage[COUNT + 1u] == 123456.0f);
    TEST_ASSERT(q_storage[0] == -123456.0f);
    TEST_ASSERT(q_storage[COUNT + 1u] == -123456.0f);
}
