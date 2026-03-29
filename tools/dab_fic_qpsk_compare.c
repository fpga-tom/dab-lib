#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dab/core/tframe.h"

typedef struct {
    char magic[8];
    uint32_t symbol_count;
    float carriers[DAB_TFRAME_FIC_SYMBOLS][2][1536];
} fic_carrier_dump_t;

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

static int8_t quantize_soft(float value) {
    int q = (int)value;

    if (q > 127) {
        q = 127;
    } else if (q < -128) {
        q = -128;
    }
    return (int8_t)q;
}

static const char *variant_name(unsigned int variant) {
    switch (variant) {
        case 0u: return "-re,-im";
        case 1u: return "re,-im";
        case 2u: return "-re,im";
        case 3u: return "re,im";
        case 4u: return "-im,-re";
        case 5u: return "im,-re";
        case 6u: return "-im,re";
        case 7u: return "im,re";
        default: return "?";
    }
}

static void map_carriers_to_softbits(const fic_carrier_dump_t *dump,
                                     unsigned int variant,
                                     fic_symbol_dump_t *out) {
    uint32_t symbol;

    memcpy(out->magic, "DABFIC1", 8u);
    out->symbol_count = DAB_TFRAME_FIC_SYMBOLS;
    for (symbol = 0u; symbol < DAB_TFRAME_FIC_SYMBOLS; ++symbol) {
        uint32_t i;

        for (i = 0u; i < 1536u; ++i) {
            const float re = dump->carriers[symbol][0][i];
            const float im = dump->carriers[symbol][1][i];
            const float mag = sqrtf((re * re) + (im * im));
            const float scale = mag > 0.0f ? (127.5f / mag) : 0.0f;
            float a;
            float b;

            switch (variant) {
                case 0u: a = -re * scale; b = -im * scale; break;
                case 1u: a =  re * scale; b = -im * scale; break;
                case 2u: a = -re * scale; b =  im * scale; break;
                case 3u: a =  re * scale; b =  im * scale; break;
                case 4u: a = -im * scale; b = -re * scale; break;
                case 5u: a =  im * scale; b = -re * scale; break;
                case 6u: a = -im * scale; b =  re * scale; break;
                default: a =  im * scale; b =  re * scale; break;
            }

            out->softbits[symbol][0][i] = quantize_soft(a);
            out->softbits[symbol][1][i] = quantize_soft(b);
        }
    }
}

int main(int argc, char **argv) {
    fic_carrier_dump_t carriers;
    fic_symbol_dump_t actual;
    fic_symbol_dump_t expected;
    unsigned int variant;

    if (argc != 3) {
        fprintf(stderr, "usage: %s /tmp/fic_carriers.bin /tmp/fic_symbols.bin\n", argv[0]);
        return EXIT_FAILURE;
    }
    if (read_binary_file(argv[1], &carriers, sizeof(carriers)) != 0) {
        fprintf(stderr, "failed to read %s\n", argv[1]);
        return EXIT_FAILURE;
    }
    if (memcmp(carriers.magic, "DABFIC2", 7u) != 0 || carriers.symbol_count != DAB_TFRAME_FIC_SYMBOLS) {
        fprintf(stderr, "unexpected carrier dump format in %s\n", argv[1]);
        return EXIT_FAILURE;
    }
    if (read_binary_file(argv[2], &actual, sizeof(actual)) != 0) {
        fprintf(stderr, "failed to read %s\n", argv[2]);
        return EXIT_FAILURE;
    }
    if (memcmp(actual.magic, "DABFIC1", 7u) != 0 || actual.symbol_count != DAB_TFRAME_FIC_SYMBOLS) {
        fprintf(stderr, "unexpected symbol dump format in %s\n", argv[2]);
        return EXIT_FAILURE;
    }

    for (variant = 0u; variant < 8u; ++variant) {
        uint32_t mismatches = 0u;
        uint32_t symbol;
        uint32_t lane;
        uint32_t i;

        map_carriers_to_softbits(&carriers, variant, &expected);
        for (symbol = 0u; symbol < DAB_TFRAME_FIC_SYMBOLS; ++symbol) {
            for (lane = 0u; lane < 2u; ++lane) {
                for (i = 0u; i < 1536u; ++i) {
                    if (expected.softbits[symbol][lane][i] != actual.softbits[symbol][lane][i]) {
                        mismatches++;
                    }
                }
            }
        }
        printf("%s mismatches=%u\n", variant_name(variant), mismatches);
    }

    return EXIT_SUCCESS;
}
