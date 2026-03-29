#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "dab/core/fic.h"
#include "dab/core/ofdm.h"
#include "dab/core/tframe.h"

typedef struct {
    char magic[8];
    uint32_t symbol_count;
    float carriers[DAB_TFRAME_FIC_SYMBOLS + 1u][2][1537];
} fic_active_dump_t;

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

static void diff_prev_conj_curr(const float *prev_i,
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
        diff_i[i] = (prev_i[i] * curr_i[i]) + (prev_q[i] * curr_q[i]);
        diff_q[i] = (prev_q[i] * curr_i[i]) - (prev_i[i] * curr_q[i]);
    }
}

static void slice_variant(const float *deintl_i,
                          const float *deintl_q,
                          unsigned int variant,
                          int8_t *soft_a,
                          int8_t *soft_b) {
    uint32_t i;

    for (i = 0u; i < DAB_MODE_I_DATA_CARRIERS; ++i) {
        float x;
        float y;
        float a;
        float b;
        float mag;
        float scale;

        switch (variant) {
            case 0u: x = -deintl_i[i]; y = -deintl_q[i]; break;
            case 1u: x =  deintl_i[i]; y = -deintl_q[i]; break;
            case 2u: x = -deintl_i[i]; y =  deintl_q[i]; break;
            case 3u: x =  deintl_i[i]; y =  deintl_q[i]; break;
            case 4u: x = -deintl_q[i]; y = -deintl_i[i]; break;
            case 5u: x =  deintl_q[i]; y = -deintl_i[i]; break;
            case 6u: x = -deintl_q[i]; y =  deintl_i[i]; break;
            default: x =  deintl_q[i]; y =  deintl_i[i]; break;
        }

        mag = sqrtf((x * x) + (y * y));
        scale = mag > 0.0f ? (-127.5f / mag) : 0.0f;
        a = x * scale;
        b = y * scale;
        if (a > 127.0f) a = 127.0f;
        if (a < -127.0f) a = -127.0f;
        if (b > 127.0f) b = 127.0f;
        if (b < -127.0f) b = -127.0f;
        soft_a[i] = (int8_t)a;
        soft_b[i] = (int8_t)b;
    }
}

static const char *diff_name(unsigned int mode) {
    switch (mode) {
        case 0u: return "curr_conj_prev_seq";
        case 1u: return "prev_conj_curr_seq";
        case 2u: return "curr_conj_prev_prs";
        default: return "prev_conj_curr_prs";
    }
}

static const char *slice_name(unsigned int variant) {
    switch (variant) {
        case 0u: return "-re,-im";
        case 1u: return "re,-im";
        case 2u: return "-re,im";
        case 3u: return "re,im";
        case 4u: return "-im,-re";
        case 5u: return "im,-re";
        case 6u: return "-im,re";
        default: return "im,re";
    }
}

int main(int argc, char **argv) {
    fic_active_dump_t dump;
    dab_ofdm_t ofdm;
    float diff_i[DAB_MODE_I_ACTIVE_CARRIERS];
    float diff_q[DAB_MODE_I_ACTIVE_CARRIERS];
    float deintl_i[DAB_MODE_I_DATA_CARRIERS];
    float deintl_q[DAB_MODE_I_DATA_CARRIERS];
    int8_t soft_a[DAB_MODE_I_DATA_CARRIERS];
    int8_t soft_b[DAB_MODE_I_DATA_CARRIERS];
    int8_t fic_softbits[DAB_TFRAME_FIC_SOFTBITS];
    unsigned int diff_mode;
    unsigned int slice_variant_idx;

    if (argc != 2) {
        fprintf(stderr, "usage: %s /path/to/fic_active_dump.bin\n", argv[0]);
        return EXIT_FAILURE;
    }

    if (read_binary_file(argv[1], &dump, sizeof(dump)) != 0) {
        fprintf(stderr, "failed to read %s\n", argv[1]);
        return EXIT_FAILURE;
    }
    if (memcmp(dump.magic, "DABFIC4", 7u) != 0 || dump.symbol_count != (DAB_TFRAME_FIC_SYMBOLS + 1u)) {
        fprintf(stderr, "unexpected dump format in %s\n", argv[1]);
        return EXIT_FAILURE;
    }
    if (dab_ofdm_init(&ofdm, 0) != 0) {
        fprintf(stderr, "ofdm init failed\n");
        return EXIT_FAILURE;
    }

    for (diff_mode = 0u; diff_mode < 4u; ++diff_mode) {
        for (slice_variant_idx = 0u; slice_variant_idx < 8u; ++slice_variant_idx) {
            dab_fic_debug_report_t report;
            uint32_t symbol;

            memset(fic_softbits, 0, sizeof(fic_softbits));
            for (symbol = 0u; symbol < DAB_TFRAME_FIC_SYMBOLS; ++symbol) {
                const float *prev_i;
                const float *prev_q;
                const float *curr_i = dump.carriers[symbol + 1u][0];
                const float *curr_q = dump.carriers[symbol + 1u][1];
                int8_t *dst = fic_softbits + (symbol * DAB_TFRAME_SOFTBITS_PER_SYMBOL);

                if ((diff_mode & 2u) != 0u || symbol == 0u) {
                    prev_i = dump.carriers[0][0];
                    prev_q = dump.carriers[0][1];
                } else {
                    prev_i = dump.carriers[symbol][0];
                    prev_q = dump.carriers[symbol][1];
                }

                if ((diff_mode & 1u) == 0u) {
                    diff_curr_conj_prev(prev_i, prev_q, curr_i, curr_q, diff_i, diff_q);
                } else {
                    diff_prev_conj_curr(prev_i, prev_q, curr_i, curr_q, diff_i, diff_q);
                }

                if (dab_ofdm_frequency_deinterleave_mode_i(&ofdm, diff_i, diff_q, deintl_i, deintl_q) != 0) {
                    fprintf(stderr, "deinterleave failed\n");
                    return EXIT_FAILURE;
                }
                slice_variant(deintl_i, deintl_q, slice_variant_idx, soft_a, soft_b);
                memcpy(dst, soft_a, 1536u * sizeof(soft_a[0]));
                memcpy(dst + 1536u, soft_b, 1536u * sizeof(soft_b[0]));
            }

            if (dab_fic_debug_analyze_frame(fic_softbits, DAB_TFRAME_FIC_SOFTBITS, &report) != 0) {
                fprintf(stderr, "fic analyze failed\n");
                return EXIT_FAILURE;
            }

            printf("%s %s total_valid_fibs=%u\n",
                   diff_name(diff_mode),
                   slice_name(slice_variant_idx),
                   report.total_valid_fibs);
        }
    }

    return EXIT_SUCCESS;
}
