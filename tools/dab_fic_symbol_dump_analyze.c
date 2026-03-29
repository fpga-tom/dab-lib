#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dab/core/fic.h"
#include "dab/core/tframe.h"

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

static void map_symbol_variant(const int8_t *src_a,
                               const int8_t *src_b,
                               unsigned int variant,
                               int8_t *dst_a,
                               int8_t *dst_b) {
    uint32_t i;
    const int swap = (variant & 4u) != 0u;
    const int invert_a = (variant & 1u) != 0u;
    const int invert_b = (variant & 2u) != 0u;

    for (i = 0u; i < 1536u; ++i) {
        const int8_t a = swap ? src_b[i] : src_a[i];
        const int8_t b = swap ? src_a[i] : src_b[i];
        dst_a[i] = invert_a ? (int8_t)-a : a;
        dst_b[i] = invert_b ? (int8_t)-b : b;
    }
}

static void build_fic_frame_from_symbols(const fic_symbol_dump_t *dump,
                                         const uint8_t *order,
                                         unsigned int variant,
                                         int8_t *fic_softbits) {
    uint32_t symbol;

    for (symbol = 0u; symbol < DAB_TFRAME_FIC_SYMBOLS; ++symbol) {
        int8_t *dst = fic_softbits + (symbol * DAB_TFRAME_SOFTBITS_PER_SYMBOL);
        const uint32_t src_symbol = order != 0 ? order[symbol] : symbol;
        map_symbol_variant(dump->softbits[src_symbol][0],
                           dump->softbits[src_symbol][1],
                           variant,
                           dst,
                           dst + 1536u);
    }
}

static const char *variant_name(unsigned int variant) {
    switch (variant) {
        case 0u: return "a,b";
        case 1u: return "-a,b";
        case 2u: return "a,-b";
        case 3u: return "-a,-b";
        case 4u: return "b,a";
        case 5u: return "-b,a";
        case 6u: return "b,-a";
        case 7u: return "-b,-a";
        default: return "?";
    }
}

int main(int argc, char **argv) {
    fic_symbol_dump_t dump;
    int8_t fic_softbits[DAB_TFRAME_FIC_SOFTBITS];
    static const uint8_t permutations[6][DAB_TFRAME_FIC_SYMBOLS] = {
        {0u, 1u, 2u},
        {0u, 2u, 1u},
        {1u, 0u, 2u},
        {1u, 2u, 0u},
        {2u, 0u, 1u},
        {2u, 1u, 0u}
    };
    unsigned int perm;
    unsigned int variant;

    if (argc != 2) {
        fprintf(stderr, "usage: %s /path/to/fic_symbols_dump.bin\n", argv[0]);
        return EXIT_FAILURE;
    }

    if (read_binary_file(argv[1], &dump, sizeof(dump)) != 0) {
        fprintf(stderr, "failed to read %s\n", argv[1]);
        return EXIT_FAILURE;
    }
    if (memcmp(dump.magic, "DABFIC1", 7u) != 0 || dump.symbol_count != DAB_TFRAME_FIC_SYMBOLS) {
        fprintf(stderr, "unexpected dump format in %s\n", argv[1]);
        return EXIT_FAILURE;
    }

    for (perm = 0u; perm < 6u; ++perm) {
        for (variant = 0u; variant < 8u; ++variant) {
            dab_fic_debug_report_t report;

            build_fic_frame_from_symbols(&dump, permutations[perm], variant, fic_softbits);
            if (dab_fic_debug_analyze_frame(fic_softbits, DAB_TFRAME_FIC_SOFTBITS, &report) != 0) {
                fprintf(stderr, "analyze failed for permutation %u variant %u\n", perm, variant);
                return EXIT_FAILURE;
            }

            printf("sym=%u%u%u %s total_valid_fibs=%u\n",
                   permutations[perm][0],
                   permutations[perm][1],
                   permutations[perm][2],
                   variant_name(variant),
                   report.total_valid_fibs);
        }
    }

    return EXIT_SUCCESS;
}
