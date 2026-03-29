#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

typedef struct {
    char magic[8];
    uint32_t symbol_count;
    uint64_t frame_start_sample;
    uint64_t symbol_sample[3];
    float carriers[3][2][1536];
} fic_carrier_dump_t;

typedef struct {
    char magic[8];
    uint32_t symbol_count;
    uint64_t symbol_sample[3];
    float carriers[3][2][1536];
} ref_fic_carrier_dump_t;

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

static void map_variant(int variant, float a, float b, float *oa, float *ob) {
    switch (variant) {
        case 0: *oa = a;  *ob = b;  break;
        case 1: *oa = -a; *ob = b;  break;
        case 2: *oa = a;  *ob = -b; break;
        case 3: *oa = -a; *ob = -b; break;
        case 4: *oa = b;  *ob = a;  break;
        case 5: *oa = -b; *ob = a;  break;
        case 6: *oa = b;  *ob = -a; break;
        case 7: *oa = -b; *ob = -a; break;
        default: *oa = a; *ob = b;  break;
    }
}

int main(int argc, char **argv) {
    fic_carrier_dump_t ours;
    ref_fic_carrier_dump_t ref;
    uint32_t symbols;
    static const uint8_t permutations[6][3] = {
        {0, 1, 2},
        {0, 2, 1},
        {1, 0, 2},
        {1, 2, 0},
        {2, 0, 1},
        {2, 1, 0}
    };
    static const char *variant_names[8] = {
        "a,b", "-a,b", "a,-b", "-a,-b", "b,a", "-b,a", "b,-a", "-b,-a"
    };
    uint32_t perm;
    uint32_t variant;
    double best_rms = 0.0;
    uint32_t best_perm = 0u;
    uint32_t best_variant = 0u;

    if (argc != 3) {
        fprintf(stderr, "usage: %s /tmp/fic_carriers.bin /tmp/ref_fic_carriers.bin\n", argv[0]);
        return 1;
    }
    if (read_exact(argv[1], &ours, sizeof(ours)) != 0 ||
        read_exact(argv[2], &ref, sizeof(ref)) != 0) {
        return 1;
    }

    symbols = ours.symbol_count < ref.symbol_count ? ours.symbol_count : ref.symbol_count;
    for (perm = 0u; perm < 6u; ++perm) {
        for (variant = 0u; variant < 8u; ++variant) {
            double sum = 0.0;
            uint64_t count = 0u;
            uint32_t s;
            uint32_t i;

            for (s = 0u; s < symbols; ++s) {
                for (i = 0u; i < 1536u; ++i) {
                    float oa;
                    float ob;
                    float da;
                    float db;
                    map_variant(variant,
                                ours.carriers[permutations[perm][s]][0][i],
                                ours.carriers[permutations[perm][s]][1][i],
                                &oa,
                                &ob);
                    da = oa - ref.carriers[s][0][i];
                    db = ob - ref.carriers[s][1][i];
                    sum += ((double)da * (double)da) + ((double)db * (double)db);
                    count += 2u;
                }
            }

            if ((perm == 0u && variant == 0u) || sum < best_rms) {
                best_rms = sum;
                best_perm = perm;
                best_variant = variant;
            }
        }
    }

    printf("carrier_dump_compare symbols=%u best_perm=%u order=%u,%u,%u map=%s rms=%.9f\n",
           symbols,
           best_perm,
           permutations[best_perm][0],
           permutations[best_perm][1],
           permutations[best_perm][2],
           variant_names[best_variant],
           symbols > 0u ? sqrt(best_rms / (double)(symbols * 1536u * 2u)) : 0.0);
    return 0;
}
