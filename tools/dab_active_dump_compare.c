#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

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
} ref_active_dump_t;

static int read_file(const char *path, void *out, size_t size) {
    FILE *fp;

    fp = fopen(path, "rb");
    if (fp == NULL) {
        return -1;
    }
    if (fread(out, 1u, size, fp) != size) {
        fclose(fp);
        return -1;
    }
    fclose(fp);
    return 0;
}

static double rms_pair(const float a[2][1537], const float b[2][1537]) {
    double acc = 0.0;
    uint32_t i;

    for (i = 0u; i < 1537u; ++i) {
        const double dre = (double)a[0][i] - (double)b[0][i];
        const double dim = (double)a[1][i] - (double)b[1][i];
        acc += dre * dre;
        acc += dim * dim;
    }
    return sqrt(acc / (double)(1537u * 2u));
}

int main(int argc, char **argv) {
    fic_active_dump_t ours;
    ref_active_dump_t ref;
    double eq_rms[3];
    double phase_rms[3];
    unsigned int i;

    if (argc != 4) {
        fprintf(stderr, "usage: %s <ours_active.bin> <ref_eq.bin> <ref_phase.bin>\n", argv[0]);
        return 1;
    }

    if (read_file(argv[1], &ours, sizeof(ours)) != 0 ||
        read_file(argv[2], &ref, sizeof(ref)) != 0) {
        fprintf(stderr, "failed to read active/eq dump\n");
        return 1;
    }

    {
        ref_active_dump_t phase;
        if (read_file(argv[3], &phase, sizeof(phase)) != 0) {
            fprintf(stderr, "failed to read phase dump\n");
            return 1;
        }
        for (i = 0u; i < 3u; ++i) {
            eq_rms[i] = rms_pair(ours.carriers[i + 1u], ref.carriers[i]);
            phase_rms[i] = rms_pair(ours.carriers[i], phase.carriers[i]);
        }
    }

    for (i = 0u; i < 3u; ++i) {
        printf("symbol=%u eq_rms=%.9f phase_rms=%.9f\n", i, eq_rms[i], phase_rms[i]);
    }

    return 0;
}
