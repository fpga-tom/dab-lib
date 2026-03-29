#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dab/core/fic.h"
#include "dab/core/tframe.h"
#include "../src/core/viterbi47_port.h"

#define REF_FIC_INFO_BITS 768u

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

static uint8_t prbs_bit(int reset) {
    static uint16_t reg = 0x1FFu;
    const uint8_t o = (uint8_t)((reg & 0x100u) != 0u);
    const uint8_t o5 = (uint8_t)((reg & 0x010u) != 0u);
    uint8_t s;

    if (reset) {
        reg = 0x1FFu;
    }
    s = (uint8_t)((o ^ o5) & 1u);
    reg = (uint16_t)(((reg << 1u) & 0x1FFu) | s);
    return s;
}

static uint8_t prbs8(uint32_t reset) {
    uint8_t value = 0u;
    uint32_t i;

    for (i = 0u; i < 8u; ++i) {
        value = (uint8_t)((value << 1u) | prbs_bit(reset != 0u && i == 0u));
    }
    return value;
}

static uint16_t crc16_ccitt(const uint8_t *buf, size_t len) {
    uint16_t crc = 0xFFFFu;
    size_t i;

    for (i = 0u; i < len; ++i) {
        uint32_t bit;

        crc ^= (uint16_t)buf[i] << 8u;
        for (bit = 0u; bit < 8u; ++bit) {
            crc = (crc & 0x8000u) != 0u ? (uint16_t)((crc << 1u) ^ 0x1021u)
                                        : (uint16_t)(crc << 1u);
        }
    }
    return (uint16_t)(~crc);
}

static int fib_crc_ok(const uint8_t fib[DAB_FIC_BYTES_PER_FIB]) {
    const uint16_t calc = crc16_ccitt(fib, 30u);
    const uint16_t rx = (uint16_t)(((uint16_t)fib[30] << 8u) | fib[31]);
    return calc == rx;
}

static int ref_decode_block_raw(const dab_fic_debug_block_dump_t *dump,
                                uint32_t polarity_variant,
                                uint8_t decoded[DAB_FIC_BYTES_PER_BLOCK]) {
    unsigned char metrics[DAB_FIC_UNPUNCTURED_BITS + 32u];
    uint32_t i;
    void *vp;

    memset(metrics, 0, sizeof(metrics));
    for (i = 0u; i < DAB_FIC_UNPUNCTURED_BITS; ++i) {
        int value = (polarity_variant == 0u ? dump->unpunctured[i] : -dump->unpunctured[i]) + 127;
        if (value < 0) {
            value = 0;
        } else if (value > 255) {
            value = 255;
        }
        metrics[i] = (unsigned char)value;
    }

    vp = create_viterbi47_port((int)REF_FIC_INFO_BITS);
    if (vp == 0) {
        return -1;
    }
    if (init_viterbi47_port(vp, 0) != 0 ||
        update_viterbi47_blk_port(vp, metrics, (int)DAB_FIC_TRELLIS_BITS) != 0 ||
        chainback_viterbi47_port(vp, decoded, REF_FIC_INFO_BITS, 0) != 0) {
        delete_viterbi47_port(vp);
        return -1;
    }
    delete_viterbi47_port(vp);

    return 0;
}

static int ref_decode_block_descrambled(const dab_fic_debug_block_dump_t *dump,
                                        uint32_t polarity_variant,
                                        uint8_t decoded[DAB_FIC_BYTES_PER_BLOCK]) {
    uint32_t i;

    if (ref_decode_block_raw(dump, polarity_variant, decoded) != 0) {
        return -1;
    }
    for (i = 0u; i < DAB_FIC_BYTES_PER_BLOCK; ++i) {
        decoded[i] ^= prbs8(i == 0u);
    }
    return 0;
}

static uint32_t count_valid_fibs(const uint8_t block[DAB_FIC_BYTES_PER_BLOCK]) {
    uint32_t valid = 0u;
    uint32_t fib_index;

    for (fib_index = 0u; fib_index < DAB_FIC_FIBS_PER_BLOCK; ++fib_index) {
        if (fib_crc_ok(block + (fib_index * DAB_FIC_BYTES_PER_FIB)) != 0) {
            valid++;
        }
    }
    return valid;
}

static void print_prefix(const char *label, const uint8_t *buf) {
    uint32_t i;
    printf("%s", label);
    for (i = 0u; i < 16u; ++i) {
        printf(" %02x", buf[i]);
    }
    printf("\n");
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
        uint8_t ref_raw[DAB_FIC_BYTES_PER_BLOCK];
        uint8_t ref_block[DAB_FIC_BYTES_PER_BLOCK];
        uint8_t ref_block_inv[DAB_FIC_BYTES_PER_BLOCK];

        if (dab_fic_debug_decode_block(fic_softbits, DAB_TFRAME_FIC_SOFTBITS, block_index, &dump) != 0) {
            fprintf(stderr, "decode failed for block %u\n", block_index);
            return EXIT_FAILURE;
        }
        if (ref_decode_block_raw(&dump, 0u, ref_raw) != 0 ||
            ref_decode_block_descrambled(&dump, 0u, ref_block) != 0 ||
            ref_decode_block_descrambled(&dump, 1u, ref_block_inv) != 0) {
            fprintf(stderr, "ref decode failed for block %u\n", block_index);
            return EXIT_FAILURE;
        }

        printf("block=%u ours_valid=%u ref_valid=%u ref_inv_valid=%u\n",
               block_index,
               dump.info.valid_fibs,
               count_valid_fibs(ref_block),
               count_valid_fibs(ref_block_inv));
        print_prefix("  raw :", dump.packed_lsb);
        print_prefix("  refr:", ref_raw);
        print_prefix("  ours:", dump.decoded_block);
        print_prefix("  ref :", ref_block);
        print_prefix("  refi:", ref_block_inv);
    }

    return EXIT_SUCCESS;
}
