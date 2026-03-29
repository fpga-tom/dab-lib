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

static uint8_t prbs81(uint32_t reset) {
    uint8_t value = 0u;
    uint32_t i;

    for (i = 0u; i < 8u; ++i) {
        value = (uint8_t)((value << 1u) | prbs_bit(reset != 0u && i == 0u));
    }
    return value;
}

static uint8_t bit_swap_8(uint8_t value) {
    value = (uint8_t)(((value & 0x55u) << 1u) | ((value & 0xAAu) >> 1u));
    value = (uint8_t)(((value & 0x33u) << 2u) | ((value & 0xCCu) >> 2u));
    value = (uint8_t)(((value & 0x0Fu) << 4u) | ((value & 0xF0u) >> 4u));
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

static uint32_t count_valid_fibs_be(const uint8_t *block) {
    uint32_t valid = 0u;
    uint32_t fib_index;

    for (fib_index = 0u; fib_index < DAB_FIC_FIBS_PER_BLOCK; ++fib_index) {
        const uint8_t *fib = block + (fib_index * DAB_FIC_BYTES_PER_FIB);
        const uint16_t calc = crc16_ccitt(fib, 30u);
        const uint16_t rx = (uint16_t)(((uint16_t)fib[30] << 8u) | fib[31]);
        if (calc == rx) {
            valid++;
        }
    }
    return valid;
}

static uint32_t count_valid_fibs_le(const uint8_t *block) {
    uint32_t valid = 0u;
    uint32_t fib_index;

    for (fib_index = 0u; fib_index < DAB_FIC_FIBS_PER_BLOCK; ++fib_index) {
        const uint8_t *fib = block + (fib_index * DAB_FIC_BYTES_PER_FIB);
        const uint16_t calc = crc16_ccitt(fib, 30u);
        const uint16_t rx = (uint16_t)(((uint16_t)fib[31] << 8u) | fib[30]);
        if (calc == rx) {
            valid++;
        }
    }
    return valid;
}

static void apply_variant(const uint8_t *raw,
                          uint32_t scramble_variant,
                          uint32_t byte_variant,
                          uint8_t *out) {
    uint32_t i;

    for (i = 0u; i < DAB_FIC_BYTES_PER_BLOCK; ++i) {
        uint8_t value = raw[i];

        if (scramble_variant == 1u) {
            value ^= prbs81(i == 0u);
        } else if (scramble_variant == 2u) {
            value ^= bit_swap_8(prbs81(i == 0u));
        } else if (scramble_variant == 3u) {
            value = bit_swap_8(value ^ prbs81(i == 0u));
        }
        if (byte_variant == 1u) {
            value = bit_swap_8(value);
        }
        out[i] = value;
    }
}

static const char *scramble_name(uint32_t v) {
    switch (v) {
        case 0u: return "none";
        case 1u: return "xor_prbs81";
        case 2u: return "xor_bitswap_prbs81";
        default: return "bitswap_after_xor_prbs81";
    }
}

static const char *byte_name(uint32_t v) {
    return v == 0u ? "identity" : "bitswap";
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
        uint32_t scramble_variant;
        uint32_t byte_variant;

        if (dab_fic_debug_decode_block(fic_softbits,
                                       DAB_TFRAME_FIC_SOFTBITS,
                                       block_index,
                                       &dump) != 0) {
            fprintf(stderr, "decode failed for block %u\n", block_index);
            return EXIT_FAILURE;
        }
        printf("block=%u\n", block_index);
        for (scramble_variant = 0u; scramble_variant < 4u; ++scramble_variant) {
            for (byte_variant = 0u; byte_variant < 2u; ++byte_variant) {
                uint8_t candidate[DAB_FIC_BYTES_PER_BLOCK];

                apply_variant(dump.packed_lsb, scramble_variant, byte_variant, candidate);
                printf("  %s + %s -> crc_be=%u crc_le=%u\n",
                       scramble_name(scramble_variant),
                       byte_name(byte_variant),
                       count_valid_fibs_be(candidate),
                       count_valid_fibs_le(candidate));
            }
        }
    }

    return EXIT_SUCCESS;
}
