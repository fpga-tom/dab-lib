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

static void build_ref_style_fic_frame(const fic_symbol_dump_t *dump, int8_t *fic_softbits) {
    uint32_t symbol;

    memset(fic_softbits, 0, DAB_TFRAME_FIC_SOFTBITS * sizeof(*fic_softbits));
    for (symbol = 0u; symbol < DAB_TFRAME_FIC_SYMBOLS; ++symbol) {
        uint32_t i;
        int8_t *dst = fic_softbits + (symbol * DAB_TFRAME_SOFTBITS_PER_SYMBOL);

        for (i = 0u; i < 1536u; ++i) {
            dst[i] = dump->softbits[symbol][0][i];
            dst[1536u + i] = dump->softbits[symbol][1][i];
        }
    }
}

static uint32_t compare_block(const int8_t *a, const int8_t *b, uint32_t block_index) {
    uint32_t mismatches = 0u;
    uint32_t i;
    const uint32_t base = block_index * DAB_FIC_DEPARTITIONED_SOFTBITS;

    for (i = 0u; i < DAB_FIC_DEPARTITIONED_SOFTBITS; ++i) {
        if (a[base + i] != b[base + i]) {
            mismatches++;
        }
    }
    return mismatches;
}

int main(int argc, char **argv) {
    fic_symbol_dump_t symbols;
    int8_t expected[DAB_TFRAME_FIC_SOFTBITS];
    int8_t assembled[DAB_TFRAME_FIC_SOFTBITS];
    uint32_t block_index;
    uint32_t total_mismatches = 0u;

    if (argc != 3) {
        fprintf(stderr, "usage: %s /tmp/fic_symbols.bin /tmp/fic_frame.bin\n", argv[0]);
        return EXIT_FAILURE;
    }

    if (read_binary_file(argv[1], &symbols, sizeof(symbols)) != 0) {
        fprintf(stderr, "failed to read %s\n", argv[1]);
        return EXIT_FAILURE;
    }
    if (memcmp(symbols.magic, "DABFIC1", 7u) != 0 || symbols.symbol_count != DAB_TFRAME_FIC_SYMBOLS) {
        fprintf(stderr, "unexpected symbol dump format in %s\n", argv[1]);
        return EXIT_FAILURE;
    }
    if (read_binary_file(argv[2], assembled, sizeof(assembled)) != 0) {
        fprintf(stderr, "failed to read %s\n", argv[2]);
        return EXIT_FAILURE;
    }

    build_ref_style_fic_frame(&symbols, expected);

    for (block_index = 0u; block_index < DAB_FIC_BLOCKS_PER_FRAME; ++block_index) {
        const uint32_t mismatches = compare_block(expected, assembled, block_index);
        total_mismatches += mismatches;
        printf("block=%u mismatches=%u\n", block_index, mismatches);
    }

    printf("total_mismatches=%u\n", total_mismatches);
    if (total_mismatches != 0u) {
        uint32_t i;
        for (i = 0u; i < DAB_TFRAME_FIC_SOFTBITS; ++i) {
            if (expected[i] != assembled[i]) {
                printf("first_diff index=%u expected=%d actual=%d\n",
                       i,
                       (int)expected[i],
                       (int)assembled[i]);
                break;
            }
        }
    }

    return EXIT_SUCCESS;
}
