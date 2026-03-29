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
} fic_raw_active_dump_t;

typedef struct {
    char magic[8];
    uint32_t symbol_count;
    uint64_t symbol_sample[3];
    float carriers[3][2][1537];
} ref_raw_dump_t;

static int read_file(const char *path, void *out, size_t size) {
    FILE *fp = fopen(path, "rb");
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
    fic_raw_active_dump_t ours;
    ref_raw_dump_t ref;
    unsigned int i;

    if (argc != 3) {
        fprintf(stderr, "usage: %s <ours_raw_active.bin> <ref_raw.bin>\n", argv[0]);
        return 1;
    }

    if (read_file(argv[1], &ours, sizeof(ours)) != 0 ||
        read_file(argv[2], &ref, sizeof(ref)) != 0) {
        fprintf(stderr, "failed to read dumps\n");
        return 1;
    }

    for (i = 0u; i < 3u; ++i) {
        printf("symbol=%u raw_rms=%.9f\n", i, rms_pair(ours.carriers[i + 1u], ref.carriers[i]));
    }
    return 0;
}
