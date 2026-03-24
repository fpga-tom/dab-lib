#include "dab/simd/simd.h"

static const dab_simd_ops_t *g_dab_simd_ops = 0;

static const dab_simd_ops_t *dab_simd_detect_best(void) {
#if defined(DAB_ENABLE_AVX2) && (defined(__AVX2__) || defined(_M_AVX2))
    return dab_simd_avx2_ops();
#elif defined(DAB_ENABLE_NEON) && (defined(__ARM_NEON) || defined(__ARM_NEON__))
    return dab_simd_neon_ops();
#else
    return dab_simd_scalar_ops();
#endif
}

void dab_simd_init(void) {
    if (g_dab_simd_ops == 0) {
        g_dab_simd_ops = dab_simd_detect_best();
    }
}

const dab_simd_ops_t *dab_simd_get_ops(void) {
    dab_simd_init();
    return g_dab_simd_ops;
}
