#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dab/core/fic.h"
#include "dab/core/tframe.h"

typedef struct {
    char magic[8];
    uint32_t symbol_count;
    float carriers[DAB_TFRAME_FIC_SYMBOLS][2][1536];
} fic_carrier_dump_t;

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

static int8_t quantize_soft(float value) {
    if (value > 127.0f) {
        value = 127.0f;
    } else if (value < -127.0f) {
        value = -127.0f;
    }
    return (int8_t)(value >= 0.0f ? (value + 0.5f) : (value - 0.5f));
}

static void map_carriers_to_softbits(const fic_carrier_dump_t *dump,
                                     unsigned int variant,
                                     int8_t *fic_softbits) {
    uint32_t symbol;

    for (symbol = 0u; symbol < DAB_TFRAME_FIC_SYMBOLS; ++symbol) {
        int8_t *dst_a = fic_softbits + (symbol * DAB_TFRAME_SOFTBITS_PER_SYMBOL);
        int8_t *dst_b = dst_a + 1536u;
        uint32_t i;

        for (i = 0u; i < 1536u; ++i) {
            const float re = dump->carriers[symbol][0][i];
            const float im = dump->carriers[symbol][1][i];
            const float mag = sqrtf((re * re) + (im * im));
            float scale = mag > 0.0f ? (127.5f / mag) : 0.0f;
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
                case 7u: a =  im * scale; b =  re * scale; break;
                default: a = 0.0f; b = 0.0f; break;
            }

            dst_a[i] = quantize_soft(a);
            dst_b[i] = quantize_soft(b);
        }
    }
}

int main(int argc, char **argv) {
    fic_carrier_dump_t dump;
    int8_t fic_softbits[DAB_TFRAME_FIC_SOFTBITS];
    unsigned int variant;

    if (argc != 2) {
        fprintf(stderr, "usage: %s /path/to/fic_carriers_dump.bin\n", argv[0]);
        return EXIT_FAILURE;
    }
    if (read_binary_file(argv[1], &dump, sizeof(dump)) != 0) {
        fprintf(stderr, "failed to read %s\n", argv[1]);
        return EXIT_FAILURE;
    }
    if (memcmp(dump.magic, "DABFIC2", 7u) != 0 || dump.symbol_count != DAB_TFRAME_FIC_SYMBOLS) {
        fprintf(stderr, "unexpected dump format in %s\n", argv[1]);
        return EXIT_FAILURE;
    }

    for (variant = 0u; variant < 8u; ++variant) {
        dab_fic_debug_report_t report;

        map_carriers_to_softbits(&dump, variant, fic_softbits);
        if (dab_fic_debug_analyze_frame(fic_softbits, DAB_TFRAME_FIC_SOFTBITS, &report) != 0) {
            fprintf(stderr, "analyze failed for variant %u\n", variant);
            return EXIT_FAILURE;
        }
        printf("%s total_valid_fibs=%u\n", variant_name(variant), report.total_valid_fibs);
    }

    return EXIT_SUCCESS;
}
