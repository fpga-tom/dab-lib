#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "dab/core/ofdm.h"

enum {
    REF_KSYM = 1537,
    REF_KSYM2 = 768,
    REF_GROUP_COUNT = 48
};

static const int REF_K[REF_GROUP_COUNT] = {
    -768, -736, -704, -672, -640, -608, -576, -544, -512, -480, -448, -416,
    -384, -352, -320, -288, -256, -224, -192, -160, -128, -96, -64, -32,
    1, 33, 65, 97, 129, 161, 193, 225, 257, 289, 321, 353, 385, 417, 449,
    481, 513, 545, 577, 609, 641, 673, 705, 737
};

static const int REF_II[REF_GROUP_COUNT] = {
    0, 1, 2, 3, 0, 1, 2, 3, 0, 1, 2, 3, 0, 1, 2, 3, 0, 1, 2, 3, 0, 1, 2, 3,
    0, 3, 2, 1, 0, 3, 2, 1, 0, 3, 2, 1, 0, 3, 2, 1, 0, 3, 2, 1, 0, 3, 2, 1
};

static const int REF_H[4][32] = {
    {0, 2, 0, 0, 0, 0, 1, 1, 2, 0, 0, 0, 2, 2, 1, 1,
     0, 2, 0, 0, 0, 0, 1, 1, 2, 0, 0, 0, 2, 2, 1, 1},
    {0, 3, 2, 3, 0, 1, 3, 0, 2, 1, 2, 3, 2, 3, 3, 0,
     0, 3, 2, 3, 0, 1, 3, 0, 2, 1, 2, 3, 2, 3, 3, 0},
    {0, 0, 0, 2, 0, 2, 1, 3, 2, 2, 0, 2, 2, 0, 1, 3,
     0, 0, 0, 2, 0, 2, 1, 3, 2, 2, 0, 2, 2, 0, 1, 3},
    {0, 1, 2, 1, 0, 3, 3, 2, 2, 3, 2, 1, 2, 1, 3, 2,
     0, 1, 2, 1, 0, 3, 3, 2, 2, 3, 2, 1, 2, 1, 3, 2}
};

static int ref_pilot_idx(int k) {
    int i;

    for (i = 0; i < REF_GROUP_COUNT; ++i) {
        if (k >= REF_K[i] && k <= (REF_K[i] + 31)) {
            return i;
        }
    }
    return -1;
}

static void build_ref_genpilot(float *ref_i, float *ref_q) {
    int k;

    memset(ref_i, 0, sizeof(*ref_i) * REF_KSYM);
    memset(ref_q, 0, sizeof(*ref_q) * REF_KSYM);

    for (k = -REF_KSYM2; k <= REF_KSYM2; ++k) {
        const int out = REF_KSYM2 + k;

        if (k == 0) {
            continue;
        }

        {
            const int idx = ref_pilot_idx(k);
            const int l = k - REF_K[idx];
            const float phase = ((float)M_PI / 2.0f) * (float)REF_H[REF_II[idx]][l];

            ref_i[out] = cosf(phase);
            ref_q[out] = sinf(phase);
        }
    }
}

int main(void) {
    dab_ofdm_t ofdm;
    dab_ofdm_config_t config;
    float ref_i[REF_KSYM];
    float ref_q[REF_KSYM];
    double sum_sq_i = 0.0;
    double sum_sq_q = 0.0;
    double max_err_i = 0.0;
    double max_err_q = 0.0;
    uint32_t mismatches = 0u;
    uint32_t i;

    dab_ofdm_default_config(&config);
    if (dab_ofdm_init(&ofdm, &config) != 0) {
        fprintf(stderr, "dab_ofdm_init failed\n");
        return 1;
    }

    build_ref_genpilot(ref_i, ref_q);

    for (i = 0u; i < REF_KSYM; ++i) {
        const double di = (double)ofdm.prs_ref_i[i] - (double)ref_i[i];
        const double dq = (double)ofdm.prs_ref_q[i] - (double)ref_q[i];
        const double ai = fabs(di);
        const double aq = fabs(dq);

        sum_sq_i += di * di;
        sum_sq_q += dq * dq;
        if (ai > max_err_i) {
            max_err_i = ai;
        }
        if (aq > max_err_q) {
            max_err_q = aq;
        }
        if (ai > 1.0e-6 || aq > 1.0e-6) {
            if (mismatches < 16u) {
                printf("idx=%u ours=(%.9f,%.9f) ref=(%.9f,%.9f)\n",
                       i,
                       ofdm.prs_ref_i[i],
                       ofdm.prs_ref_q[i],
                       ref_i[i],
                       ref_q[i]);
            }
            mismatches++;
        }
    }

    printf("prs_ref_compare mismatches=%u rms_i=%.9g rms_q=%.9g max_i=%.9g max_q=%.9g\n",
           mismatches,
           sqrt(sum_sq_i / (double)REF_KSYM),
           sqrt(sum_sq_q / (double)REF_KSYM),
           max_err_i,
           max_err_q);

    return 0;
}
