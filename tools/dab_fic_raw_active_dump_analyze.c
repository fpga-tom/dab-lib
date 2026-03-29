#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "dab/core/fic.h"
#include "dab/core/ofdm.h"
#include "dab/core/tframe.h"

#include "prs_template_mode_i_table.h"

typedef struct {
    char magic[8];
    uint32_t symbol_count;
    float carriers[DAB_TFRAME_FIC_SYMBOLS + 1u][2][1537];
} fic_raw_active_dump_t;

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
        const float mag = (float)sqrt((re * re) + (im * im));
        const float scale = mag > 0.0f ? (-127.5f / mag) : 0.0f;
        int a = (int)(re * scale);
        int b = (int)(im * scale);

        if (a > 127) {
            a = 127;
        } else if (a < -128) {
            a = -128;
        }
        if (b > 127) {
            b = 127;
        } else if (b < -128) {
            b = -128;
        }
        soft_a[i] = (int8_t)a;
        soft_b[i] = (int8_t)b;
    }
}

static void remap_active_variant(const float *src_i,
                                 const float *src_q,
                                 unsigned int variant,
                                 float *dst_i,
                                 float *dst_q) {
    uint32_t i;

    switch (variant) {
        case 0u:
            memcpy(dst_i, src_i, sizeof(float) * DAB_MODE_I_ACTIVE_CARRIERS);
            memcpy(dst_q, src_q, sizeof(float) * DAB_MODE_I_ACTIVE_CARRIERS);
            break;
        case 1u:
            for (i = 0u; i < DAB_MODE_I_ACTIVE_CARRIER_HALF; ++i) {
                dst_i[i] = src_i[DAB_MODE_I_ACTIVE_CARRIER_HALF + 1u + i];
                dst_q[i] = src_q[DAB_MODE_I_ACTIVE_CARRIER_HALF + 1u + i];
                dst_i[DAB_MODE_I_ACTIVE_CARRIER_HALF + 1u + i] = src_i[i];
                dst_q[DAB_MODE_I_ACTIVE_CARRIER_HALF + 1u + i] = src_q[i];
            }
            dst_i[DAB_MODE_I_ACTIVE_CARRIER_HALF] = src_i[DAB_MODE_I_ACTIVE_CARRIER_HALF];
            dst_q[DAB_MODE_I_ACTIVE_CARRIER_HALF] = src_q[DAB_MODE_I_ACTIVE_CARRIER_HALF];
            break;
        case 2u:
            for (i = 0u; i < DAB_MODE_I_ACTIVE_CARRIERS; ++i) {
                dst_i[i] = src_i[(i + 1u) % DAB_MODE_I_ACTIVE_CARRIERS];
                dst_q[i] = src_q[(i + 1u) % DAB_MODE_I_ACTIVE_CARRIERS];
            }
            break;
        default:
            for (i = 0u; i < DAB_MODE_I_ACTIVE_CARRIERS; ++i) {
                dst_i[(i + 1u) % DAB_MODE_I_ACTIVE_CARRIERS] = src_i[i];
                dst_q[(i + 1u) % DAB_MODE_I_ACTIVE_CARRIERS] = src_q[i];
            }
            break;
    }
    dst_i[DAB_MODE_I_ACTIVE_CARRIER_HALF] = 0.0f;
    dst_q[DAB_MODE_I_ACTIVE_CARRIER_HALF] = 0.0f;
}

static const char *mode_name(unsigned int mode) {
    switch (mode) {
        case 0u: return "eq_seq";
        case 1u: return "eq_prs";
        case 2u: return "raw_seq";
        default: return "raw_prs";
    }
}

static const char *order_name(unsigned int variant) {
    switch (variant) {
        case 0u: return "identity";
        case 1u: return "swap_halves";
        case 2u: return "rotate_left_1";
        default: return "rotate_right_1";
    }
}

int main(int argc, char **argv) {
    fic_raw_active_dump_t dump;
    dab_ofdm_t ofdm;
    float prs_i[DAB_MODE_I_ACTIVE_CARRIERS];
    float prs_q[DAB_MODE_I_ACTIVE_CARRIERS];
    float map_prs_i[DAB_MODE_I_ACTIVE_CARRIERS];
    float map_prs_q[DAB_MODE_I_ACTIVE_CARRIERS];
    float chan_i[DAB_MODE_I_ACTIVE_CARRIERS];
    float chan_q[DAB_MODE_I_ACTIVE_CARRIERS];
    float eq_data_i[DAB_TFRAME_FIC_SYMBOLS][DAB_MODE_I_ACTIVE_CARRIERS];
    float eq_data_q[DAB_TFRAME_FIC_SYMBOLS][DAB_MODE_I_ACTIVE_CARRIERS];
    float map_data_i[DAB_TFRAME_FIC_SYMBOLS][DAB_MODE_I_ACTIVE_CARRIERS];
    float map_data_q[DAB_TFRAME_FIC_SYMBOLS][DAB_MODE_I_ACTIVE_CARRIERS];
    float diff_i[DAB_MODE_I_ACTIVE_CARRIERS];
    float diff_q[DAB_MODE_I_ACTIVE_CARRIERS];
    float deintl_i[DAB_MODE_I_DATA_CARRIERS];
    float deintl_q[DAB_MODE_I_DATA_CARRIERS];
    int8_t soft_a[DAB_MODE_I_DATA_CARRIERS];
    int8_t soft_b[DAB_MODE_I_DATA_CARRIERS];
    int8_t fic_softbits[DAB_TFRAME_FIC_SOFTBITS];
    unsigned int mode;
    unsigned int order_variant;

    if (argc != 2) {
        fprintf(stderr, "usage: %s /tmp/fic_raw_active.bin\n", argv[0]);
        return EXIT_FAILURE;
    }
    if (read_binary_file(argv[1], &dump, sizeof(dump)) != 0) {
        fprintf(stderr, "failed to read %s\n", argv[1]);
        return EXIT_FAILURE;
    }
    if (memcmp(dump.magic, "DABFIC5", 7u) != 0 || dump.symbol_count != (DAB_TFRAME_FIC_SYMBOLS + 1u)) {
        fprintf(stderr, "unexpected dump format in %s\n", argv[1]);
        return EXIT_FAILURE;
    }
    if (dab_ofdm_init(&ofdm, 0) != 0) {
        fprintf(stderr, "ofdm init failed\n");
        return EXIT_FAILURE;
    }

    init_prs_reference(prs_i, prs_q);
    for (order_variant = 0u; order_variant < 4u; ++order_variant) {
        remap_active_variant(prs_i, prs_q, order_variant, map_prs_i, map_prs_q);
        estimate_channel(map_prs_i, map_prs_q, dump.carriers[0][0], dump.carriers[0][1], chan_i, chan_q);
        for (mode = 0u; mode < 4u; ++mode) {
            dab_fic_debug_report_t report;
            uint32_t symbol;

            memset(fic_softbits, 0, sizeof(fic_softbits));
            for (symbol = 0u; symbol < DAB_TFRAME_FIC_SYMBOLS; ++symbol) {
                const float *curr_i;
                const float *curr_q;
                const float *prev_i;
                const float *prev_q;
                int8_t *dst = fic_softbits + (symbol * DAB_TFRAME_SOFTBITS_PER_SYMBOL);

                remap_active_variant(dump.carriers[symbol + 1u][0],
                                     dump.carriers[symbol + 1u][1],
                                     order_variant,
                                     map_data_i[symbol],
                                     map_data_q[symbol]);
                if (mode < 2u) {
                    equalize_symbol(map_data_i[symbol],
                                    map_data_q[symbol],
                                    chan_i,
                                    chan_q,
                                    eq_data_i[symbol],
                                    eq_data_q[symbol]);
                    curr_i = eq_data_i[symbol];
                    curr_q = eq_data_q[symbol];
                } else {
                    curr_i = map_data_i[symbol];
                    curr_q = map_data_q[symbol];
                }

                if ((mode & 1u) != 0u || symbol == 0u) {
                    prev_i = (mode < 2u) ? map_prs_i : dump.carriers[0][0];
                    prev_q = (mode < 2u) ? map_prs_q : dump.carriers[0][1];
                } else {
                    prev_i = eq_data_i[symbol - 1u];
                    prev_q = eq_data_q[symbol - 1u];
                }

                diff_curr_conj_prev(prev_i, prev_q, curr_i, curr_q, diff_i, diff_q);
                if (dab_ofdm_frequency_deinterleave_mode_i(&ofdm, diff_i, diff_q, deintl_i, deintl_q) != 0) {
                    fprintf(stderr, "deinterleave failed\n");
                    return EXIT_FAILURE;
                }
                slice_softbits(deintl_i, deintl_q, soft_a, soft_b);
                memcpy(dst, soft_a, 1536u * sizeof(soft_a[0]));
                memcpy(dst + 1536u, soft_b, 1536u * sizeof(soft_b[0]));
            }

            if (dab_fic_debug_analyze_frame(fic_softbits, DAB_TFRAME_FIC_SOFTBITS, &report) != 0) {
                fprintf(stderr, "fic analyze failed\n");
                return EXIT_FAILURE;
            }
            printf("%s %s total_valid_fibs=%u\n",
                   order_name(order_variant),
                   mode_name(mode),
                   report.total_valid_fibs);
        }
    }

    return EXIT_SUCCESS;
}
