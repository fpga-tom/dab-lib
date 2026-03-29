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

static uint32_t puncturing_code(uint32_t p) {
    switch (p) {
        case 15u: return 0x37777777u;
        case 16u: return 0x77777777u;
        case 25u: return 0x00333333u;
        default: return 0u;
    }
}

static uint32_t getpcfic(uint32_t bit_addr) {
    uint32_t low = 0u;

    if (bit_addr >= low && bit_addr < low + (21u * 128u)) {
        return puncturing_code(16u);
    }
    low += 21u * 128u;
    if (bit_addr >= low && bit_addr < low + (3u * 128u)) {
        return puncturing_code(15u);
    }
    low += 3u * 128u;
    if (bit_addr >= low && bit_addr < low + 24u) {
        return puncturing_code(25u);
    }
    return 0u;
}

static void build_reference_unpunctured(const int16_t *depunctured, int16_t *unpunctured) {
    uint32_t dec = 0u;
    uint32_t l = 0u;
    uint32_t code_bits = 0u;
    uint32_t out_pos = 0u;

    while (dec < DAB_FIC_DEPARTITIONED_SOFTBITS && out_pos < DAB_FIC_UNPUNCTURED_BITS) {
        uint32_t pc = getpcfic(l);
        uint32_t i;

        if (pc == 0u) {
            break;
        }
        code_bits = 0u;
        for (i = 0u; i < 32u && out_pos < DAB_FIC_UNPUNCTURED_BITS; ++i) {
            if (((pc >> i) & 1u) != 0u) {
                unpunctured[out_pos++] = depunctured[dec++];
                code_bits++;
            } else {
                unpunctured[out_pos++] = 0;
            }
        }
        l += 32u;
    }
}

int main(int argc, char **argv) {
    int8_t fic_softbits[DAB_TFRAME_FIC_SOFTBITS];
    uint32_t block_index;

    if (argc != 2) {
        fprintf(stderr, "usage: %s /tmp/fic_frame.bin\n", argv[0]);
        return EXIT_FAILURE;
    }
    if (read_binary_file(argv[1], fic_softbits, sizeof(fic_softbits)) != 0) {
        fprintf(stderr, "failed to read %s\n", argv[1]);
        return EXIT_FAILURE;
    }

    for (block_index = 0u; block_index < DAB_FIC_BLOCKS_PER_FRAME; ++block_index) {
        dab_fic_debug_block_dump_t dump;
        int16_t ref_unpunctured[DAB_FIC_UNPUNCTURED_BITS];
        uint32_t mismatches = 0u;
        uint32_t i;

        if (dab_fic_debug_decode_block(fic_softbits,
                                       DAB_TFRAME_FIC_SOFTBITS,
                                       block_index,
                                       &dump) != 0) {
            fprintf(stderr, "decode failed for block %u\n", block_index);
            return EXIT_FAILURE;
        }

        memset(ref_unpunctured, 0, sizeof(ref_unpunctured));
        build_reference_unpunctured(dump.depunctured, ref_unpunctured);
        for (i = 0u; i < DAB_FIC_UNPUNCTURED_BITS; ++i) {
            if (dump.unpunctured[i] != ref_unpunctured[i]) {
                mismatches++;
            }
        }
        printf("block=%u unpuncture_mismatches=%u\n", block_index, mismatches);
        if (mismatches != 0u) {
            for (i = 0u; i < DAB_FIC_UNPUNCTURED_BITS; ++i) {
                if (dump.unpunctured[i] != ref_unpunctured[i]) {
                    printf("  first_diff index=%u ours=%d ref=%d\n",
                           i,
                           (int)dump.unpunctured[i],
                           (int)ref_unpunctured[i]);
                    break;
                }
            }
        }
    }

    return EXIT_SUCCESS;
}
