#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dab/core/ofdm.h"

#include "prs_template_mode_i_table.h"

typedef struct {
    char magic[8];
    uint32_t symbol_count;
    float carriers[4][2][1537];
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

static const char *order_name(unsigned int variant) {
    switch (variant) {
        case 0u: return "identity";
        case 1u: return "swap_halves";
        case 2u: return "rotate_left_1";
        default: return "rotate_right_1";
    }
}

static float prs_adjacent_score(const float *rx_i,
                                const float *rx_q,
                                const float *ref_i,
                                const float *ref_q) {
    double sum_re = 0.0;
    double sum_im = 0.0;
    double rx_energy = 0.0;
    double ref_energy = 0.0;
    uint32_t i;

    for (i = 0u; i + 1u < DAB_MODE_I_ACTIVE_CARRIERS; ++i) {
        double rx_re;
        double rx_im;
        double ref_re;
        double ref_im;

        if (i == DAB_MODE_I_ACTIVE_CARRIER_HALF - 1u ||
            i == DAB_MODE_I_ACTIVE_CARRIER_HALF ||
            i + 1u == DAB_MODE_I_ACTIVE_CARRIER_HALF) {
            continue;
        }
        rx_re = ((double)rx_i[i] * rx_i[i + 1u]) + ((double)rx_q[i] * rx_q[i + 1u]);
        rx_im = ((double)rx_q[i] * rx_i[i + 1u]) - ((double)rx_i[i] * rx_q[i + 1u]);
        ref_re = ((double)ref_i[i] * ref_i[i + 1u]) + ((double)ref_q[i] * ref_q[i + 1u]);
        ref_im = ((double)ref_q[i] * ref_i[i + 1u]) - ((double)ref_i[i] * ref_q[i + 1u]);
        sum_re += (rx_re * ref_re) + (rx_im * ref_im);
        sum_im += (rx_im * ref_re) - (rx_re * ref_im);
        rx_energy += (rx_re * rx_re) + (rx_im * rx_im);
        ref_energy += (ref_re * ref_re) + (ref_im * ref_im);
    }

    if (rx_energy <= 0.0 || ref_energy <= 0.0) {
        return 0.0f;
    }
    return (float)(((sum_re * sum_re) + (sum_im * sum_im)) / (rx_energy * ref_energy));
}

static float channel_smoothness_score(const float *rx_i,
                                      const float *rx_q,
                                      const float *ref_i,
                                      const float *ref_q) {
    double accum = 0.0;
    uint32_t count = 0u;
    float prev_h_re = 0.0f;
    float prev_h_im = 0.0f;
    int have_prev = 0;
    uint32_t i;

    for (i = 0u; i < DAB_MODE_I_ACTIVE_CARRIERS; ++i) {
        float denom;
        float h_re;
        float h_im;
        float d_re;
        float d_im;

        if (i == DAB_MODE_I_ACTIVE_CARRIER_HALF) {
            continue;
        }
        denom = (ref_i[i] * ref_i[i]) + (ref_q[i] * ref_q[i]);
        if (denom < 1.0e-12f) {
            continue;
        }
        h_re = ((rx_i[i] * ref_i[i]) + (rx_q[i] * ref_q[i])) / denom;
        h_im = ((rx_q[i] * ref_i[i]) - (rx_i[i] * ref_q[i])) / denom;
        if (have_prev) {
            d_re = h_re - prev_h_re;
            d_im = h_im - prev_h_im;
            accum += sqrt((double)(d_re * d_re) + (double)(d_im * d_im));
            count++;
        }
        prev_h_re = h_re;
        prev_h_im = h_im;
        have_prev = 1;
    }

    if (count == 0u) {
        return 0.0f;
    }
    return (float)(accum / count);
}

int main(int argc, char **argv) {
    fic_raw_active_dump_t dump;
    float prs_i[DAB_MODE_I_ACTIVE_CARRIERS];
    float prs_q[DAB_MODE_I_ACTIVE_CARRIERS];
    float map_i[DAB_MODE_I_ACTIVE_CARRIERS];
    float map_q[DAB_MODE_I_ACTIVE_CARRIERS];
    unsigned int order_variant;

    if (argc != 2) {
        fprintf(stderr, "usage: %s /tmp/fic_raw_active.bin\n", argv[0]);
        return EXIT_FAILURE;
    }
    if (read_binary_file(argv[1], &dump, sizeof(dump)) != 0) {
        fprintf(stderr, "failed to read %s\n", argv[1]);
        return EXIT_FAILURE;
    }
    if (memcmp(dump.magic, "DABFIC5", 7u) != 0 || dump.symbol_count != 4u) {
        fprintf(stderr, "unexpected dump format in %s\n", argv[1]);
        return EXIT_FAILURE;
    }

    init_prs_reference(prs_i, prs_q);
    for (order_variant = 0u; order_variant < 4u; ++order_variant) {
        float adj_score;
        float smooth_score;
        remap_active_variant(dump.carriers[0][0],
                             dump.carriers[0][1],
                             order_variant,
                             map_i,
                             map_q);
        adj_score = prs_adjacent_score(map_i, map_q, prs_i, prs_q);
        smooth_score = channel_smoothness_score(map_i, map_q, prs_i, prs_q);
        printf("%s adjacent_score=%.6f channel_smoothness=%.6f\n",
               order_name(order_variant),
               adj_score,
               smooth_score);
    }

    return EXIT_SUCCESS;
}
