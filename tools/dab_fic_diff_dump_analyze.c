#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dab/core/fic.h"
#include "dab/core/tframe.h"

typedef struct {
    char magic[8];
    uint32_t symbol_count;
    float carriers[DAB_TFRAME_FIC_SYMBOLS][2][1537];
} fic_diff_dump_t;

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

static void build_deinterleave_map(uint16_t *map) {
    uint32_t p = 0u;
    uint32_t i;
    uint16_t k = 0u;

    for (i = 0u; i < 2048u; ++i) {
        const uint32_t a = p;
        p = (13u * p + 511u) % 2048u;
        if (a >= 256u && a <= 1792u && a != 1024u) {
            map[k++] = (uint16_t)(a - 256u);
            if (k >= 1536u) {
                break;
            }
        }
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

static const char *order_name(unsigned int order_variant) {
    switch (order_variant) {
        case 0u: return "identity";
        case 1u: return "identity_rev";
        case 2u: return "deintl";
        case 3u: return "deintl_rev";
        default: return "?";
    }
}

static const char *map_name(unsigned int map_variant) {
    switch (map_variant) {
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

static void select_ordered_carriers(const fic_diff_dump_t *dump,
                                    const uint16_t *deintl_map,
                                    unsigned int order_variant,
                                    unsigned int symbol,
                                    float *out_i,
                                    float *out_q) {
    uint32_t i;

    for (i = 0u; i < 1536u; ++i) {
        uint32_t src;
        switch (order_variant) {
            case 0u: src = i < 768u ? i : (i + 1u); break;
            case 1u: {
                const uint32_t base = 1535u - i;
                src = base < 768u ? base : (base + 1u);
                break;
            }
            case 2u: src = deintl_map[i]; break;
            case 3u: src = deintl_map[1535u - i]; break;
            default: src = deintl_map[i]; break;
        }
        out_i[i] = dump->carriers[symbol][0][src];
        out_q[i] = dump->carriers[symbol][1][src];
    }
}

static void build_softbits_from_diff(const fic_diff_dump_t *dump,
                                     const uint16_t *deintl_map,
                                     unsigned int order_variant,
                                     unsigned int map_variant,
                                     int8_t *fic_softbits) {
    uint32_t symbol;
    float ordered_i[1536];
    float ordered_q[1536];

    for (symbol = 0u; symbol < DAB_TFRAME_FIC_SYMBOLS; ++symbol) {
        int8_t *dst_a = fic_softbits + (symbol * DAB_TFRAME_SOFTBITS_PER_SYMBOL);
        int8_t *dst_b = dst_a + 1536u;
        uint32_t i;

        select_ordered_carriers(dump, deintl_map, order_variant, symbol, ordered_i, ordered_q);
        for (i = 0u; i < 1536u; ++i) {
            const float re = ordered_i[i];
            const float im = ordered_q[i];
            const float mag = sqrtf((re * re) + (im * im));
            const float scale = mag > 0.0f ? (127.5f / mag) : 0.0f;
            float a;
            float b;

            switch (map_variant) {
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
    fic_diff_dump_t dump;
    uint16_t deintl_map[1536];
    int8_t fic_softbits[DAB_TFRAME_FIC_SOFTBITS];
    unsigned int order_variant;
    unsigned int map_variant;

    if (argc != 2) {
        fprintf(stderr, "usage: %s /path/to/fic_diff_dump.bin\n", argv[0]);
        return EXIT_FAILURE;
    }
    if (read_binary_file(argv[1], &dump, sizeof(dump)) != 0) {
        fprintf(stderr, "failed to read %s\n", argv[1]);
        return EXIT_FAILURE;
    }
    if (memcmp(dump.magic, "DABFIC3", 7u) != 0 || dump.symbol_count != DAB_TFRAME_FIC_SYMBOLS) {
        fprintf(stderr, "unexpected dump format in %s\n", argv[1]);
        return EXIT_FAILURE;
    }

    build_deinterleave_map(deintl_map);

    for (order_variant = 0u; order_variant < 4u; ++order_variant) {
        for (map_variant = 0u; map_variant < 8u; ++map_variant) {
            dab_fic_debug_report_t report;

            build_softbits_from_diff(&dump, deintl_map, order_variant, map_variant, fic_softbits);
            if (dab_fic_debug_analyze_frame(fic_softbits, DAB_TFRAME_FIC_SOFTBITS, &report) != 0) {
                fprintf(stderr, "analyze failed\n");
                return EXIT_FAILURE;
            }

            if (report.total_valid_fibs > 0u) {
                printf("%s + %s total_valid_fibs=%u\n",
                       order_name(order_variant),
                       map_name(map_variant),
                       report.total_valid_fibs);
            }
        }
    }

    return EXIT_SUCCESS;
}
