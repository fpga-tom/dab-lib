#include "dab/simd/simd.h"

const dab_simd_ops_t *dab_simd_avx2_ops(void) {
    return dab_simd_scalar_ops();
}
