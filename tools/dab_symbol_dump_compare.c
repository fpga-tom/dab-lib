#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    char magic[8];
    uint32_t symbol_count;
    int8_t softbits[3][2][1536];
} fic_symbol_dump_t;

typedef struct {
    char magic[8];
    uint32_t symbol_count;
    int32_t softbits[3][2][1536];
} ref_fic_symbol_dump_t;

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

static int32_t clamp_i8(int32_t v) {
    if (v > 127) {
        return 127;
    }
    if (v < -128) {
        return -128;
    }
    return v;
}

static void map_variant(int variant, int32_t a, int32_t b, int32_t *oa, int32_t *ob) {
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
    fic_symbol_dump_t ours;
    ref_fic_symbol_dump_t ref;
    uint32_t symbols;
    uint64_t exact_mismatches = 0u;
    uint64_t clipped_mismatches = 0u;
    uint64_t sign_mismatches = 0u;
    int32_t ref_max = 0;
    int32_t ref_min = 0;
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
    uint64_t best_mismatches = 0u;
    uint32_t best_perm = 0u;
    uint32_t best_variant = 0u;
    uint32_t s;
    uint32_t ab;
    uint32_t i;

    if (argc != 3) {
        fprintf(stderr, "usage: %s /tmp/fic_symbols.bin /tmp/ref_fic_symbols.bin\n", argv[0]);
        return 1;
    }

    if (read_exact(argv[1], &ours, sizeof(ours)) != 0 ||
        read_exact(argv[2], &ref, sizeof(ref)) != 0) {
        return 1;
    }

    symbols = ours.symbol_count < ref.symbol_count ? ours.symbol_count : ref.symbol_count;
    for (s = 0u; s < symbols; ++s) {
        for (ab = 0u; ab < 2u; ++ab) {
            for (i = 0u; i < 1536u; ++i) {
                int32_t o = ours.softbits[s][ab][i];
                int32_t r = ref.softbits[s][ab][i];
                int32_t rc = clamp_i8(r);
                if ((s == 0u && ab == 0u && i == 0u) || r > ref_max) {
                    ref_max = r;
                }
                if ((s == 0u && ab == 0u && i == 0u) || r < ref_min) {
                    ref_min = r;
                }
                if (o != r) {
                    exact_mismatches++;
                }
                if (o != rc) {
                    clipped_mismatches++;
                }
                if (((o > 0) - (o < 0)) != ((r > 0) - (r < 0))) {
                    sign_mismatches++;
                }
            }
        }
    }

    printf("symbol_dump_compare symbols=%u exact_mismatches=%llu clipped_mismatches=%llu sign_mismatches=%llu ref_min=%d ref_max=%d\n",
           symbols,
           (unsigned long long)exact_mismatches,
           (unsigned long long)clipped_mismatches,
           (unsigned long long)sign_mismatches,
           ref_min,
           ref_max);

    if (symbols == 3u) {
        uint32_t perm;
        uint32_t variant;

        for (perm = 0u; perm < 6u; ++perm) {
            for (variant = 0u; variant < 8u; ++variant) {
                uint64_t mismatches = 0u;

                for (s = 0u; s < 3u; ++s) {
                    for (i = 0u; i < 1536u; ++i) {
                        int32_t oa;
                        int32_t ob;
                        map_variant(variant,
                                    ours.softbits[permutations[perm][s]][0][i],
                                    ours.softbits[permutations[perm][s]][1][i],
                                    &oa,
                                    &ob);
                        if (oa != ref.softbits[s][0][i]) {
                            mismatches++;
                        }
                        if (ob != ref.softbits[s][1][i]) {
                            mismatches++;
                        }
                    }
                }

                if (perm == 0u && variant == 0u || mismatches < best_mismatches) {
                    best_mismatches = mismatches;
                    best_perm = perm;
                    best_variant = variant;
                }
            }
        }

        printf("best_variant perm=%u order=%u,%u,%u map=%s mismatches=%llu\n",
               best_perm,
               permutations[best_perm][0],
               permutations[best_perm][1],
               permutations[best_perm][2],
               variant_names[best_variant],
               (unsigned long long)best_mismatches);
    }
    return 0;
}
