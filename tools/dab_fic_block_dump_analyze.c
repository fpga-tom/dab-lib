#include <stdio.h>
#include <stdlib.h>

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

static void print_hex_bytes(const char *label, const uint8_t *buf, uint32_t count) {
    uint32_t i;

    printf("%s", label);
    for (i = 0u; i < count; ++i) {
        printf(" %02x", buf[i]);
    }
    printf("\n");
}

static void print_softints(const char *label, const int16_t *buf, uint32_t count) {
    uint32_t i;

    printf("%s", label);
    for (i = 0u; i < count; ++i) {
        printf(" %d", (int)buf[i]);
    }
    printf("\n");
}

int main(int argc, char **argv) {
    int8_t fic_softbits[DAB_TFRAME_FIC_SOFTBITS];
    uint32_t block_index;

    if (argc != 2) {
        fprintf(stderr, "usage: %s /path/to/fic_dump.bin\n", argv[0]);
        return EXIT_FAILURE;
    }

    if (read_binary_file(argv[1], fic_softbits, sizeof(fic_softbits)) != 0) {
        fprintf(stderr, "failed to read %s\n", argv[1]);
        return EXIT_FAILURE;
    }

    for (block_index = 0u; block_index < DAB_FIC_BLOCKS_PER_FRAME; ++block_index) {
        dab_fic_debug_block_dump_t dump;
        uint32_t fib_index;

        if (dab_fic_debug_decode_block(fic_softbits,
                                       DAB_TFRAME_FIC_SOFTBITS,
                                       block_index,
                                       &dump) != 0) {
            fprintf(stderr, "decode failed for block %u\n", block_index);
            return EXIT_FAILURE;
        }

        printf("block=%u polarity=%u pack=%u scramble=%u byte=%u valid_fibs=%u\n",
               dump.info.block_index,
               dump.info.polarity_variant,
               dump.info.pack_variant,
               dump.info.scramble_variant,
               dump.info.byte_variant,
               dump.info.valid_fibs);
        print_softints("  depunctured[0..15]:", dump.depunctured, 16u);
        print_softints("  unpunctured[0..15]:", dump.unpunctured, 16u);
        print_hex_bytes("  packed_lsb[0..15]:", dump.packed_lsb, 16u);
        print_hex_bytes("  packed_msb[0..15]:", dump.packed_msb, 16u);
        print_hex_bytes("  decoded[0..15]:", dump.decoded_block, 16u);

        for (fib_index = 0u; fib_index < DAB_FIC_FIBS_PER_BLOCK; ++fib_index) {
            const uint8_t *fib = dump.decoded_block + (fib_index * DAB_FIC_BYTES_PER_FIB);
            printf("  fib=%u crc=%s header=%02x\n",
                   fib_index,
                   ((((uint16_t)fib[30] << 8u) | fib[31]) != 0u) ? "set" : "zero",
                   fib[0]);
        }
    }

    return EXIT_SUCCESS;
}
