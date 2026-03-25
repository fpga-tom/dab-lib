#ifndef DAB_CORE_PRS_FFT_BACKEND_H
#define DAB_CORE_PRS_FFT_BACKEND_H

#include <stdint.h>

#include "dab/core/prs_fft.h"

typedef void (*dab_prs_fft_stage_fn_t)(float *re,
                                       float *im,
                                       const float *tw_re,
                                       const float *tw_im,
                                       uint32_t fft_samples,
                                       uint32_t half_size);

typedef struct {
    dab_prs_fft_backend_kind_t kind;
    const char *name;
    dab_prs_fft_stage_fn_t stage_fn;
} dab_prs_fft_backend_t;

const dab_prs_fft_backend_t *dab_prs_fft_scalar_backend(void);
const dab_prs_fft_backend_t *dab_prs_fft_avx2_backend(void);
const dab_prs_fft_backend_t *dab_prs_fft_neon_backend(void);

#endif
