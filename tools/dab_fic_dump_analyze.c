#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dab/core/fic.h"
#include "dab/core/tframe.h"

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

static void make_planar_copy(const int8_t *src, int8_t *dst) {
    memcpy(dst, src, sizeof(*dst) * DAB_TFRAME_FIC_SOFTBITS);
}

static void make_interleaved_copy(const int8_t *src, int8_t *dst) {
    uint32_t symbol;
    for (symbol = 0u; symbol < DAB_TFRAME_FIC_SYMBOLS; ++symbol) {
        uint32_t i;
        const int8_t *src_sym = src + (symbol * DAB_TFRAME_SOFTBITS_PER_SYMBOL);
        int8_t *dst_sym = dst + (symbol * DAB_TFRAME_SOFTBITS_PER_SYMBOL);
        for (i = 0u; i < 1536u; ++i) {
            dst_sym[2u * i] = src_sym[i];
            dst_sym[(2u * i) + 1u] = src_sym[1536u + i];
        }
    }
}

static void make_swapped_halves_copy(const int8_t *src, int8_t *dst) {
    uint32_t symbol;
    for (symbol = 0u; symbol < DAB_TFRAME_FIC_SYMBOLS; ++symbol) {
        uint32_t i;
        const int8_t *src_sym = src + (symbol * DAB_TFRAME_SOFTBITS_PER_SYMBOL);
        int8_t *dst_sym = dst + (symbol * DAB_TFRAME_SOFTBITS_PER_SYMBOL);
        for (i = 0u; i < 1536u; ++i) {
            dst_sym[i] = src_sym[1536u + i];
            dst_sym[1536u + i] = src_sym[i];
        }
    }
}

static void invert_softbits(int8_t *buf) {
    uint32_t i;
    for (i = 0u; i < DAB_TFRAME_FIC_SOFTBITS; ++i) {
        buf[i] = (int8_t)-buf[i];
    }
}

static void print_report(const char *label, const dab_fic_debug_report_t *report) {
    uint32_t i;

    printf("%s total_valid_fibs=%u\n", label, report->total_valid_fibs);
    for (i = 0u; i < DAB_FIC_BLOCKS_PER_FRAME; ++i) {
        printf("  block=%u valid_fibs=%u polarity=%u pack=%u\n",
               report->blocks[i].block_index,
               report->blocks[i].valid_fibs,
               report->blocks[i].polarity_variant,
               report->blocks[i].pack_variant);
    }
}

static int analyze_variant(const char *label, const int8_t *softbits) {
    dab_fic_debug_report_t report;

    if (dab_fic_debug_analyze_frame(softbits, DAB_TFRAME_FIC_SOFTBITS, &report) != 0) {
        fprintf(stderr, "analyze failed for %s\n", label);
        return -1;
    }

    print_report(label, &report);
    return 0;
}

int main(int argc, char **argv) {
    int8_t planar[DAB_TFRAME_FIC_SOFTBITS];
    int8_t work[DAB_TFRAME_FIC_SOFTBITS];

    if (argc != 2) {
        fprintf(stderr, "usage: %s /path/to/fic_dump.bin\n", argv[0]);
        return EXIT_FAILURE;
    }

    if (read_binary_file(argv[1], planar, sizeof(planar)) != 0) {
        fprintf(stderr, "failed to read %s\n", argv[1]);
        return EXIT_FAILURE;
    }

    if (analyze_variant("planar", planar) != 0) {
        return EXIT_FAILURE;
    }

    make_planar_copy(planar, work);
    invert_softbits(work);
    if (analyze_variant("planar_inverted", work) != 0) {
        return EXIT_FAILURE;
    }

    make_interleaved_copy(planar, work);
    if (analyze_variant("interleaved", work) != 0) {
        return EXIT_FAILURE;
    }

    make_interleaved_copy(planar, work);
    invert_softbits(work);
    if (analyze_variant("interleaved_inverted", work) != 0) {
        return EXIT_FAILURE;
    }

    make_swapped_halves_copy(planar, work);
    if (analyze_variant("swapped_halves", work) != 0) {
        return EXIT_FAILURE;
    }

    make_swapped_halves_copy(planar, work);
    invert_softbits(work);
    if (analyze_variant("swapped_halves_inverted", work) != 0) {
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}
