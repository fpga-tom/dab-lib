#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dab/core/ofdm.h"
#include "dab/core/tframe.h"

#include "prs_template_mode_i_table.h"

typedef struct {
    char magic[8];
    uint32_t symbol_count;
    float carriers[DAB_TFRAME_FIC_SYMBOLS + 1u][2][1537];
} fic_active_dump_t;

typedef struct {
    char magic[8];
    uint32_t symbol_count;
    float carriers[DAB_TFRAME_FIC_SYMBOLS + 1u][2][1537];
} fic_raw_active_dump_t;

typedef struct {
    char magic[8];
    uint32_t symbol_count;
    float carriers[DAB_TFRAME_FIC_SYMBOLS][2][1537];
} fic_diff_dump_t;

typedef struct {
    char magic[8];
    uint32_t symbol_count;
    float carriers[DAB_TFRAME_FIC_SYMBOLS][2][1536];
} fic_carrier_dump_t;

typedef struct {
    char magic[8];
    uint32_t symbol_count;
    int8_t softbits[DAB_TFRAME_FIC_SYMBOLS][2][1536];
} fic_symbol_dump_t;

static int read_binary_file(const char *path, void *data, size_t size) {
    FILE *fp;

    if (path == 0 || data == 0) {
        return -1;
    }
    fp = fopen(path, "rb");
    if (fp == 0) {
        return -1;
    }
    if (fread(data, 1u, size, fp) != size) {
        fclose(fp);
        return -1;
    }
    fclose(fp);
    return 0;
}

static int active_index_from_fft_bin(uint32_t bin, uint32_t *index) {
    if (index == 0) {
        return -1;
    }
    if (bin >= (DAB_MODE_I_FFT_SAMPLES - DAB_MODE_I_ACTIVE_CARRIER_HALF)) {
        *index = bin - (DAB_MODE_I_FFT_SAMPLES - DAB_MODE_I_ACTIVE_CARRIER_HALF);
        return 0;
    }
    if (bin <= DAB_MODE_I_ACTIVE_CARRIER_HALF) {
        *index = DAB_MODE_I_ACTIVE_CARRIER_HALF + bin;
        return 0;
    }
    return -1;
}

static void init_prs_reference(float *prs_i, float *prs_q) {
    uint32_t i;

    memset(prs_i, 0, sizeof(float) * DAB_MODE_I_ACTIVE_CARRIERS);
    memset(prs_q, 0, sizeof(float) * DAB_MODE_I_ACTIVE_CARRIERS);
    for (i = 0u; i < DAB_PRS_MODE_I_BIN_COUNT; ++i) {
        uint32_t active_index = 0u;

        if (active_index_from_fft_bin(DAB_PRS_MODE_I_BINS[i].bin, &active_index) == 0 &&
            active_index < DAB_MODE_I_ACTIVE_CARRIERS) {
            prs_i[active_index] = DAB_PRS_MODE_I_BINS[i].re;
            prs_q[active_index] = DAB_PRS_MODE_I_BINS[i].im;
        }
    }
    prs_i[DAB_MODE_I_ACTIVE_CARRIER_HALF] = 0.0f;
    prs_q[DAB_MODE_I_ACTIVE_CARRIER_HALF] = 0.0f;
}

static void estimate_channel(const float *ref_i,
                             const float *ref_q,
                             const float *sym_i,
                             const float *sym_q,
                             float *chan_i,
                             float *chan_q) {
    uint32_t i;

    for (i = 0u; i < DAB_MODE_I_ACTIVE_CARRIERS; ++i) {
        const float denom = (sym_i[i] * sym_i[i]) + (sym_q[i] * sym_q[i]);

        if (i == DAB_MODE_I_ACTIVE_CARRIER_HALF || denom < 1.0e-12f) {
            chan_i[i] = 0.0f;
            chan_q[i] = 0.0f;
            continue;
        }
        chan_i[i] = ((ref_i[i] * sym_i[i]) + (ref_q[i] * sym_q[i])) / denom;
        chan_q[i] = ((ref_q[i] * sym_i[i]) - (ref_i[i] * sym_q[i])) / denom;
    }
}

static void equalize_symbol(const float *in_i,
                            const float *in_q,
                            const float *chan_i,
                            const float *chan_q,
                            float *out_i,
                            float *out_q) {
    uint32_t i;

    for (i = 0u; i < DAB_MODE_I_ACTIVE_CARRIERS; ++i) {
        if (i == DAB_MODE_I_ACTIVE_CARRIER_HALF) {
            out_i[i] = 0.0f;
            out_q[i] = 0.0f;
            continue;
        }
        out_i[i] = (in_i[i] * chan_i[i]) - (in_q[i] * chan_q[i]);
        out_q[i] = (in_i[i] * chan_q[i]) + (in_q[i] * chan_i[i]);
    }
}

static void diff_curr_conj_prev(const float *prev_i,
                                const float *prev_q,
                                const float *curr_i,
                                const float *curr_q,
                                float *diff_i,
                                float *diff_q) {
    uint32_t i;

    for (i = 0u; i < DAB_MODE_I_ACTIVE_CARRIERS; ++i) {
        if (i == DAB_MODE_I_ACTIVE_CARRIER_HALF) {
            diff_i[i] = 0.0f;
            diff_q[i] = 0.0f;
            continue;
        }
        diff_i[i] = (curr_i[i] * prev_i[i]) + (curr_q[i] * prev_q[i]);
        diff_q[i] = (curr_q[i] * prev_i[i]) - (curr_i[i] * prev_q[i]);
    }
}

static void slice_softbits(const float *deintl_i,
                           const float *deintl_q,
                           int8_t *soft_a,
                           int8_t *soft_b) {
    uint32_t i;

    for (i = 0u; i < DAB_MODE_I_DATA_CARRIERS; ++i) {
        const float re = deintl_i[i];
        const float im = deintl_q[i];
        const float mag = sqrtf((re * re) + (im * im));
        const float scale = mag > 0.0f ? (-127.5f / mag) : 0.0f;
        int a = (int)(re * scale);
        int b = (int)(im * scale);

        if (a > 127) a = 127;
        else if (a < -128) a = -128;
        if (b > 127) b = 127;
        else if (b < -128) b = -128;
        soft_a[i] = (int8_t)a;
        soft_b[i] = (int8_t)b;
    }
}

static void compare_float_vec(const char *label,
                              const float *a,
                              const float *b,
                              uint32_t count) {
    double sum_sq = 0.0;
    double max_abs = 0.0;
    uint32_t i;

    for (i = 0u; i < count; ++i) {
        const double d = (double)a[i] - (double)b[i];
        const double ad = fabs(d);
        sum_sq += d * d;
        if (ad > max_abs) {
            max_abs = ad;
        }
    }
    printf("%s rms=%.6f max=%.6f\n",
           label,
           sqrt(sum_sq / (double)count),
           max_abs);
}

static void compare_soft_vec(const char *label,
                             const int8_t *a,
                             const int8_t *b,
                             uint32_t count) {
    uint32_t mismatches = 0u;
    uint32_t i;

    for (i = 0u; i < count; ++i) {
        if (a[i] != b[i]) {
            mismatches++;
        }
    }
    printf("%s mismatches=%u\n", label, mismatches);
}

int main(int argc, char **argv) {
    fic_raw_active_dump_t raw_dump;
    fic_active_dump_t active_dump;
    fic_diff_dump_t diff_dump;
    fic_carrier_dump_t carrier_dump;
    fic_symbol_dump_t symbol_dump;
    dab_ofdm_t ofdm;
    float prs_i[DAB_MODE_I_ACTIVE_CARRIERS];
    float prs_q[DAB_MODE_I_ACTIVE_CARRIERS];
    float chan_i[DAB_MODE_I_ACTIVE_CARRIERS];
    float chan_q[DAB_MODE_I_ACTIVE_CARRIERS];
    float eq_i[DAB_MODE_I_ACTIVE_CARRIERS];
    float eq_q[DAB_MODE_I_ACTIVE_CARRIERS];
    float diff_i[DAB_MODE_I_ACTIVE_CARRIERS];
    float diff_q[DAB_MODE_I_ACTIVE_CARRIERS];
    float deintl_i[DAB_MODE_I_DATA_CARRIERS];
    float deintl_q[DAB_MODE_I_DATA_CARRIERS];
    int8_t soft_a[DAB_MODE_I_DATA_CARRIERS];
    int8_t soft_b[DAB_MODE_I_DATA_CARRIERS];
    uint32_t symbol;

    if (argc != 6) {
        fprintf(stderr,
                "usage: %s /tmp/fic_raw_active.bin /tmp/fic_active.bin /tmp/fic_diff.bin /tmp/fic_carriers.bin /tmp/fic_symbols.bin\n",
                argv[0]);
        return EXIT_FAILURE;
    }
    if (read_binary_file(argv[1], &raw_dump, sizeof(raw_dump)) != 0 ||
        read_binary_file(argv[2], &active_dump, sizeof(active_dump)) != 0 ||
        read_binary_file(argv[3], &diff_dump, sizeof(diff_dump)) != 0 ||
        read_binary_file(argv[4], &carrier_dump, sizeof(carrier_dump)) != 0 ||
        read_binary_file(argv[5], &symbol_dump, sizeof(symbol_dump)) != 0) {
        fprintf(stderr, "failed to read one or more dump files\n");
        return EXIT_FAILURE;
    }
    if (dab_ofdm_init(&ofdm, 0) != 0) {
        fprintf(stderr, "ofdm init failed\n");
        return EXIT_FAILURE;
    }

    init_prs_reference(prs_i, prs_q);
    estimate_channel(prs_i, prs_q, raw_dump.carriers[0][0], raw_dump.carriers[0][1], chan_i, chan_q);

    compare_float_vec("prs_eq_i", prs_i, active_dump.carriers[0][0], DAB_MODE_I_ACTIVE_CARRIERS);
    compare_float_vec("prs_eq_q", prs_q, active_dump.carriers[0][1], DAB_MODE_I_ACTIVE_CARRIERS);

    for (symbol = 0u; symbol < DAB_TFRAME_FIC_SYMBOLS; ++symbol) {
        const float *prev_i = (symbol == 0u) ? prs_i : active_dump.carriers[symbol][0];
        const float *prev_q = (symbol == 0u) ? prs_q : active_dump.carriers[symbol][1];

        equalize_symbol(raw_dump.carriers[symbol + 1u][0],
                        raw_dump.carriers[symbol + 1u][1],
                        chan_i,
                        chan_q,
                        eq_i,
                        eq_q);
        diff_curr_conj_prev(prev_i, prev_q, eq_i, eq_q, diff_i, diff_q);
        if (dab_ofdm_frequency_deinterleave_mode_i(&ofdm, diff_i, diff_q, deintl_i, deintl_q) != 0) {
            fprintf(stderr, "deinterleave failed\n");
            return EXIT_FAILURE;
        }
        slice_softbits(deintl_i, deintl_q, soft_a, soft_b);

        printf("symbol=%u\n", symbol);
        compare_float_vec("  active_i", eq_i, active_dump.carriers[symbol + 1u][0], DAB_MODE_I_ACTIVE_CARRIERS);
        compare_float_vec("  active_q", eq_q, active_dump.carriers[symbol + 1u][1], DAB_MODE_I_ACTIVE_CARRIERS);
        compare_float_vec("  diff_i", diff_i, diff_dump.carriers[symbol][0], DAB_MODE_I_ACTIVE_CARRIERS);
        compare_float_vec("  diff_q", diff_q, diff_dump.carriers[symbol][1], DAB_MODE_I_ACTIVE_CARRIERS);
        compare_float_vec("  deintl_i", deintl_i, carrier_dump.carriers[symbol][0], DAB_MODE_I_DATA_CARRIERS);
        compare_float_vec("  deintl_q", deintl_q, carrier_dump.carriers[symbol][1], DAB_MODE_I_DATA_CARRIERS);
        compare_soft_vec("  soft_a", soft_a, symbol_dump.softbits[symbol][0], DAB_MODE_I_DATA_CARRIERS);
        compare_soft_vec("  soft_b", soft_b, symbol_dump.softbits[symbol][1], DAB_MODE_I_DATA_CARRIERS);
    }

    return EXIT_SUCCESS;
}
