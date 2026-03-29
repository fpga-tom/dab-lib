#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dab/core/ofdm.h"
#include "dab/core/prs_fft.h"
#include "dab/hal/sdr_device.h"

typedef struct {
    char magic[8];
    uint32_t symbol_count;
    uint64_t frame_start_sample;
    uint64_t symbol_sample[4];
    float carriers[4][2][1537];
} fic_active_dump_t;

typedef struct {
    char magic[8];
    uint32_t symbol_count;
    uint64_t symbol_sample[3];
    float carriers[3][2][1537];
} ref_fic_eq_dump_t;

static int read_exact(const char *path, void *data, size_t size) {
    FILE *fp = fopen(path, "rb");
    size_t got;

    if (fp == NULL) {
        perror(path);
        return -1;
    }
    got = fread(data, 1u, size, fp);
    fclose(fp);
    if (got != size) {
        fprintf(stderr, "short read: %s\n", path);
        return -1;
    }
    return 0;
}

static int load_samples_i16(const char *path,
                            int16_t **iq_out,
                            uint32_t *sample_count_out) {
    FILE *fp = NULL;
    long bytes;
    size_t float_count;
    float *scratch = NULL;
    int16_t *iq = NULL;
    uint32_t i;

    fp = fopen(path, "rb");
    if (fp == NULL) {
        perror(path);
        return -1;
    }
    if (fseek(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        return -1;
    }
    bytes = ftell(fp);
    if (bytes < 0 || (bytes % (long)(sizeof(float) * 2u)) != 0) {
        fclose(fp);
        return -1;
    }
    if (fseek(fp, 0, SEEK_SET) != 0) {
        fclose(fp);
        return -1;
    }

    float_count = (size_t)bytes / sizeof(float);
    scratch = (float *)malloc(float_count * sizeof(float));
    iq = (int16_t *)malloc(float_count * sizeof(int16_t));
    if (scratch == NULL || iq == NULL) {
        free(scratch);
        free(iq);
        fclose(fp);
        return -1;
    }
    if (fread(scratch, sizeof(float), float_count, fp) != float_count) {
        free(scratch);
        free(iq);
        fclose(fp);
        return -1;
    }
    fclose(fp);

    for (i = 0u; i < float_count; ++i) {
        iq[i] = (int16_t)scratch[i];
    }
    free(scratch);

    *iq_out = iq;
    *sample_count_out = (uint32_t)(float_count / 2u);
    return 0;
}

static double norm_compare(const float *a_re,
                           const float *a_im,
                           const float *b_re,
                           const float *b_im,
                           uint32_t count) {
    double sum = 0.0;
    uint32_t i;

    for (i = 0u; i < count; ++i) {
        const double amag = hypot((double)a_re[i], (double)a_im[i]);
        const double bmag = hypot((double)b_re[i], (double)b_im[i]);
        double ar = 0.0;
        double ai = 0.0;
        double br = 0.0;
        double bi = 0.0;
        double dr;
        double di;

        if (amag > 1.0e-12) {
            ar = (double)a_re[i] / amag;
            ai = (double)a_im[i] / amag;
        }
        if (bmag > 1.0e-12) {
            br = (double)b_re[i] / bmag;
            bi = (double)b_im[i] / bmag;
        }
        dr = ar - br;
        di = ai - bi;
        sum += dr * dr + di * di;
    }

    return sqrt(sum / (double)(count * 2u));
}

int main(int argc, char **argv) {
    const char *samples_path;
    const char *ours_active_path;
    const char *ref_eq_path;
    int16_t *iq = NULL;
    uint32_t sample_count = 0u;
    dab_sdr_block_view_t block = {0};
    fic_active_dump_t ours;
    ref_fic_eq_dump_t ref;
    dab_prs_fft_t fft;
    dab_prs_fft_config_t fft_config;
    dab_ofdm_t ofdm;
    dab_ofdm_config_t ofdm_config;
    float fft_i[2048];
    float fft_q[2048];
    float active_i[1537];
    float active_q[1537];
    float diff_i[1537];
    float diff_q[1537];
    float deintl_i[1536];
    float deintl_q[1536];
    int8_t soft_a[1536];
    int8_t soft_b[1536];
    dab_ofdm_buffers_t buffers;
    dab_ofdm_result_t result;
    int offset_min = -600;
    int offset_max = 600;
    int best_offset = 0;
    double best_rms = 0.0;
    float cfo_hz = -179.0f;
    int offset;

    if (argc < 4) {
        fprintf(stderr,
                "usage: %s <samples.bin> <ours_fic_active.bin> <ref_fic_eq.bin> [cfo_hz] [offset_min] [offset_max]\n",
                argv[0]);
        return 1;
    }

    samples_path = argv[1];
    ours_active_path = argv[2];
    ref_eq_path = argv[3];
    if (argc >= 5) {
        cfo_hz = strtof(argv[4], NULL);
    }
    if (argc >= 6) {
        offset_min = (int)strtol(argv[5], NULL, 10);
    }
    if (argc >= 7) {
        offset_max = (int)strtol(argv[6], NULL, 10);
    }

    if (read_exact(ours_active_path, &ours, sizeof(ours)) != 0 ||
        read_exact(ref_eq_path, &ref, sizeof(ref)) != 0 ||
        load_samples_i16(samples_path, &iq, &sample_count) != 0) {
        free(iq);
        return 1;
    }

    block.iq_interleaved = iq;
    block.sample_count = sample_count;
    block.sample_index = 0u;
    block.timestamp_ns = 0u;

    dab_prs_fft_default_config(&fft_config);
    if (dab_prs_fft_init(&fft, &fft_config) != 0) {
        free(iq);
        return 1;
    }
    dab_ofdm_default_config(&ofdm_config);
    if (dab_ofdm_init(&ofdm, &ofdm_config) != 0) {
        free(iq);
        return 1;
    }
    ofdm.prev_active_i = malloc(sizeof(active_i));
    ofdm.prev_active_q = malloc(sizeof(active_q));
    if (ofdm.prev_active_i == NULL || ofdm.prev_active_q == NULL) {
        free(ofdm.prev_active_i);
        free(ofdm.prev_active_q);
        free(iq);
        return 1;
    }

    buffers.active_i = active_i;
    buffers.active_q = active_q;
    buffers.diff_i = diff_i;
    buffers.diff_q = diff_q;
    buffers.deintl_i = deintl_i;
    buffers.deintl_q = deintl_q;
    buffers.soft_a = soft_a;
    buffers.soft_b = soft_b;

    for (offset = offset_min; offset <= offset_max; ++offset) {
        uint64_t prs_start = ours.symbol_sample[0] + (int64_t)offset;
        double rms_sum = 0.0;
        int ok = 1;
        int s;

        dab_ofdm_reset(&ofdm);

        if (dab_prs_fft_execute_i16_cfo(&fft, &block, prs_start, cfo_hz, fft_i, fft_q) != 0 ||
            dab_ofdm_process_mode_i(&ofdm,
                                    fft_i,
                                    fft_q,
                                    prs_start,
                                    DAB_OFDM_SYMBOL_PRS,
                                    &buffers,
                                    &result) != 0) {
            continue;
        }

        for (s = 0; s < 3; ++s) {
            const uint64_t data_start = ours.symbol_sample[s + 1] + (int64_t)offset;
            if (dab_prs_fft_execute_i16_cfo(&fft, &block, data_start, cfo_hz, fft_i, fft_q) != 0 ||
                dab_ofdm_process_mode_i(&ofdm,
                                        fft_i,
                                        fft_q,
                                        data_start,
                                        DAB_OFDM_SYMBOL_DATA,
                                        &buffers,
                                        &result) != 0) {
                ok = 0;
                break;
            }
            rms_sum += norm_compare(active_i,
                                    active_q,
                                    ref.carriers[s][0],
                                    ref.carriers[s][1],
                                    1537u);
        }

        if (ok) {
            const double avg = rms_sum / 3.0;
            if (offset == offset_min || avg < best_rms) {
                best_rms = avg;
                best_offset = offset;
            }
        }
    }

    printf("fft_offset_scan best_offset=%d best_norm_eq_rms=%.9f prs_start=%llu ref_symbol0=%llu ours_symbol0=%llu\n",
           best_offset,
           best_rms,
           (unsigned long long)ours.symbol_sample[0],
           (unsigned long long)ref.symbol_sample[0],
           (unsigned long long)ours.symbol_sample[1]);

    free(ofdm.prev_active_i);
    free(ofdm.prev_active_q);
    free(iq);
    return 0;
}
