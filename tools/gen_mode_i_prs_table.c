#include <math.h>
#include <stdio.h>

enum {
    DAB_MODE_I_FFT_SAMPLES = 2048,
    DAB_MODE_I_KSYM2 = 768,
    DAB_MODE_I_CARRIER_COUNT = 48
};

static const float DAB_PI = 3.14159265358979323846f;

static const int K[] = {
    -768, -736, -704, -672, -640, -608, -576, -544, -512, -480, -448, -416,
    -384, -352, -320, -288, -256, -224, -192, -160, -128, -96, -64, -32,
    1, 33, 65, 97, 129, 161, 193, 225, 257, 289, 321, 353, 385, 417, 449,
    481, 513, 545, 577, 609, 641, 673, 705, 737
};

static const int II[] = {
    0, 1, 2, 3, 0, 1, 2, 3, 0, 1, 2, 3, 0, 1, 2, 3, 0, 1, 2, 3, 0, 1, 2, 3,
    0, 3, 2, 1, 0, 3, 2, 1, 0, 3, 2, 1, 0, 3, 2, 1, 0, 3, 2, 1, 0, 3, 2, 1
};

static const int H[4][32] = {
    {0, 2, 0, 0, 0, 0, 1, 1, 2, 0, 0, 0, 2, 2, 1, 1,
     0, 2, 0, 0, 0, 0, 1, 1, 2, 0, 0, 0, 2, 2, 1, 1},
    {0, 3, 2, 3, 0, 1, 3, 0, 2, 1, 2, 3, 2, 3, 3, 0,
     0, 3, 2, 3, 0, 1, 3, 0, 2, 1, 2, 3, 2, 3, 3, 0},
    {0, 0, 0, 2, 0, 2, 1, 3, 2, 2, 0, 2, 2, 0, 1, 3,
     0, 0, 0, 2, 0, 2, 1, 3, 2, 2, 0, 2, 2, 0, 1, 3},
    {0, 1, 2, 1, 0, 3, 3, 2, 2, 3, 2, 1, 2, 1, 3, 2,
     0, 1, 2, 1, 0, 3, 3, 2, 2, 3, 2, 1, 2, 1, 3, 2}
};

static int pilot_idx(int k) {
    int i;

    for (i = 0; i < DAB_MODE_I_CARRIER_COUNT; ++i) {
        if (k >= K[i] && k <= (K[i] + 31)) {
            return i;
        }
    }
    return -1;
}

static unsigned int fft_bin_from_carrier(int k) {
    return k < 0 ? (unsigned int)(DAB_MODE_I_FFT_SAMPLES + k) : (unsigned int)k;
}

int main(void) {
    int k;

    puts("#include \"dab/core/prs_template.h\"");
    puts("");
    puts("static const dab_prs_template_bin_t DAB_PRS_MODE_I_BINS[] = {");

    for (k = -DAB_MODE_I_KSYM2; k <= DAB_MODE_I_KSYM2; ++k) {
        int idx;
        int l;
        float phase;
        float re;
        float im;

        if (k == 0) {
            continue;
        }

        idx = pilot_idx(k);
        if (idx < 0) {
            continue;
        }

        l = k - K[idx];
        phase = (DAB_PI / 2.0f) * (float)H[II[idx]][l];
        re = cosf(phase);
        im = sinf(phase);

        printf("    {%u, %.9ff, %.9ff},\n", fft_bin_from_carrier(k), re, im);
    }

    puts("};");
    puts("");
    puts("static const uint32_t DAB_PRS_MODE_I_BIN_COUNT =");
    puts("    (uint32_t)(sizeof(DAB_PRS_MODE_I_BINS) / sizeof(DAB_PRS_MODE_I_BINS[0]));");

    return 0;
}
