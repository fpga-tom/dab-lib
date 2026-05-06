#include <assert.h>
#include <math.h>

#include "dab/core/ofdm.h"

static const float kPi = 3.14159265358979323846f;

static const int TEST_K[48] = {
    -768, -736, -704, -672, -640, -608, -576, -544, -512, -480, -448, -416,
    -384, -352, -320, -288, -256, -224, -192, -160, -128, -96, -64, -32,
    1, 33, 65, 97, 129, 161, 193, 225, 257, 289, 321, 353, 385, 417, 449,
    481, 513, 545, 577, 609, 641, 673, 705, 737
};

static const int TEST_II[48] = {
    0, 1, 2, 3, 0, 1, 2, 3, 0, 1, 2, 3, 0, 1, 2, 3, 0, 1, 2, 3, 0, 1, 2, 3,
    0, 3, 2, 1, 0, 3, 2, 1, 0, 3, 2, 1, 0, 3, 2, 1, 0, 3, 2, 1, 0, 3, 2, 1
};

static const int TEST_H[4][32] = {
    {0, 2, 0, 0, 0, 0, 1, 1, 2, 0, 0, 0, 2, 2, 1, 1,
     0, 2, 0, 0, 0, 0, 1, 1, 2, 0, 0, 0, 2, 2, 1, 1},
    {0, 3, 2, 3, 0, 1, 3, 0, 2, 1, 2, 3, 2, 3, 3, 0,
     0, 3, 2, 3, 0, 1, 3, 0, 2, 1, 2, 3, 2, 3, 3, 0},
    {0, 0, 0, 2, 0, 2, 1, 3, 2, 2, 0, 2, 2, 0, 1, 3,
     0, 0, 0, 2, 0, 2, 1, 3, 2, 2, 0, 2, 2, 0, 1, 3},
    {0, 1, 2, 1, 0, 3, 3, 2, 2, 3, 2, 1, 2, 1, 3, 2,
     0, 1, 2, 1, 0, 3, 3, 2, 2, 3, 2, 1, 2, 1, 3, 2}
};

static int test_pilot_idx(int k) {
    int i;

    for (i = 0; i < 48; ++i) {
        if (k >= TEST_K[i] && k <= TEST_K[i] + 31) {
            return i;
        }
    }
    return -1;
}

void test_prs_ref_matches_reference_generator(void) {
    dab_ofdm_t ofdm;
    dab_ofdm_config_t config;
    int k;

    dab_ofdm_default_config(&config);
    assert(dab_ofdm_init(&ofdm, &config) == 0);

    for (k = -768; k <= 768; ++k) {
        const int out = 768 + k;
        float ref_re = 0.0f;
        float ref_im = 0.0f;

        if (k != 0) {
            const int idx = test_pilot_idx(k);
            const int l = k - TEST_K[idx];
            const float phase = (kPi / 2.0f) * (float)TEST_H[TEST_II[idx]][l];

            ref_re = cosf(phase);
            ref_im = sinf(phase);
        }

        assert(fabsf(ofdm.prs_ref_i[out] - ref_re) < 1.0e-6f);
        assert(fabsf(ofdm.prs_ref_q[out] - ref_im) < 1.0e-6f);
    }
}
