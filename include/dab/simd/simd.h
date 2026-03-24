#ifndef DAB_SIMD_SIMD_H
#define DAB_SIMD_SIMD_H

#include <stdint.h>

typedef enum {
    DAB_SIMD_KIND_SCALAR = 0,
    DAB_SIMD_KIND_AVX2 = 1,
    DAB_SIMD_KIND_NEON = 2
} dab_simd_kind_t;

typedef struct {
    dab_simd_kind_t kind;
    const char *name;
    void (*complex_i16_to_f32)(float *dst_i, float *dst_q, const int16_t *src, uint32_t count);
    float (*dot_f32)(const float *a, const float *b, uint32_t count);
} dab_simd_ops_t;

const dab_simd_ops_t *dab_simd_get_ops(void);
void dab_simd_init(void);

const dab_simd_ops_t *dab_simd_scalar_ops(void);
const dab_simd_ops_t *dab_simd_avx2_ops(void);
const dab_simd_ops_t *dab_simd_neon_ops(void);

#endif
