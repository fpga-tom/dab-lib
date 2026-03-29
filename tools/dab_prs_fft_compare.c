#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dab/core/ofdm.h"

#include "prs_template_mode_i_table.h"

typedef struct {
    char magic[8];
    uint32_t fft_samples;
    float bins[2][2048];
} prs_fft_dump_t;

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

static void transform_fft_variant(const prs_fft_dump_t *dump,
                                  unsigned int variant,
                                  float *re,
                                  float *im) {
    uint32_t i;

    switch (variant) {
        case 0u:
            memcpy(re, dump->bins[0], sizeof(float) * 2048u);
            memcpy(im, dump->bins[1], sizeof(float) * 2048u);
            break;
        case 1u:
            for (i = 0u; i < 2048u; ++i) {
                re[i] = dump->bins[0][i];
                im[i] = -dump->bins[1][i];
            }
            break;
        case 2u:
            for (i = 0u; i < 2048u; ++i) {
                re[i] = dump->bins[0][(2048u - i) % 2048u];
                im[i] = dump->bins[1][(2048u - i) % 2048u];
            }
            break;
        default:
            for (i = 0u; i < 2048u; ++i) {
                re[i] = dump->bins[0][(2048u - i) % 2048u];
                im[i] = -dump->bins[1][(2048u - i) % 2048u];
            }
            break;
    }
}

static const char *variant_name(unsigned int variant) {
    switch (variant) {
        case 0u: return "identity";
        case 1u: return "conjugate";
        case 2u: return "reverse";
        default: return "reverse_conjugate";
    }
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

int main(int argc, char **argv) {
    prs_fft_dump_t dump;
    dab_ofdm_t ofdm;
    float fft_i[2048];
    float fft_q[2048];
    float active_i[DAB_MODE_I_ACTIVE_CARRIERS];
    float active_q[DAB_MODE_I_ACTIVE_CARRIERS];
    float prs_i[DAB_MODE_I_ACTIVE_CARRIERS];
    float prs_q[DAB_MODE_I_ACTIVE_CARRIERS];
    unsigned int variant;

    if (argc != 2) {
        fprintf(stderr, "usage: %s /tmp/prs_fft.bin\n", argv[0]);
        return EXIT_FAILURE;
    }
    if (read_binary_file(argv[1], &dump, sizeof(dump)) != 0) {
        fprintf(stderr, "failed to read %s\n", argv[1]);
        return EXIT_FAILURE;
    }
    if (memcmp(dump.magic, "DABPRSF", 7u) != 0 || dump.fft_samples != 2048u) {
        fprintf(stderr, "unexpected dump format in %s\n", argv[1]);
        return EXIT_FAILURE;
    }
    if (dab_ofdm_init(&ofdm, 0) != 0) {
        fprintf(stderr, "ofdm init failed\n");
        return EXIT_FAILURE;
    }
    init_prs_reference(prs_i, prs_q);

    for (variant = 0u; variant < 4u; ++variant) {
        transform_fft_variant(&dump, variant, fft_i, fft_q);
        if (dab_ofdm_extract_active_carriers_mode_i(fft_i, fft_q, active_i, active_q) != 0) {
            fprintf(stderr, "active extraction failed\n");
            return EXIT_FAILURE;
        }
        printf("%s adjacent_score=%.6f\n",
               variant_name(variant),
               prs_adjacent_score(active_i, active_q, prs_i, prs_q));
    }

    return EXIT_SUCCESS;
}
