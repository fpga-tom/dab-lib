#include "dab/core/msc.h"

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "viterbi47_port.h"

typedef struct {
    uint16_t size_cu;
    uint16_t bitrate_kbps;
    uint8_t protection_level;
    uint8_t valid;
    uint8_t l[4];
    int8_t pi[4];
    uint8_t padding;
    uint16_t ic;
} dab_msc_uep_entry_t;

struct dab_msc_dabplus_stream_runtime {
    uint16_t cif_bytes;
    uint16_t row_count;
    uint16_t input_row;
    uint16_t columns_filled;
    uint8_t synchronized;
    uint8_t superframe_valid;
    uint64_t generation;
    uint8_t dac_rate;
    uint8_t sbr_flag;
    uint8_t aac_channel_mode;
    uint8_t ps_flag;
    uint8_t mpeg_surround_config;
    uint8_t au_count;
    uint32_t superframe_bytes;
    uint32_t au_start[DAB_MSC_DABPLUS_MAX_AUS];
    uint32_t au_length[DAB_MSC_DABPLUS_MAX_AUS];
    uint32_t decode_attempts;
    uint32_t decode_failures;
    uint32_t rs_corrected_last_superframe;
    uint32_t rs_corrected_total;
    uint8_t last_decode_ok;
    uint8_t *columns;
    uint8_t *superframe;
};

struct dab_msc_decoder_workspace {
    int8_t *deinterleaved;
    int16_t *unpunctured;
    unsigned char *metrics;
    void *viterbi;
    uint32_t conv_len;
    uint32_t unpunctured_count;
    uint32_t framebits;
};

static const dab_msc_uep_entry_t DAB_MSC_UEP_TABLE[64] = {
    {16u, 32u, 5u, 1u, {3u, 4u, 17u, 0u}, {5, 3, 2, -1}, 0u, 768u},
    {21u, 32u, 4u, 1u, {3u, 3u, 18u, 0u}, {11, 6, 5, -1}, 0u, 768u},
    {24u, 32u, 3u, 1u, {3u, 4u, 14u, 3u}, {15, 9, 6, 8}, 0u, 768u},
    {29u, 32u, 2u, 1u, {3u, 4u, 14u, 3u}, {22, 13, 8, 13}, 0u, 768u},
    {35u, 32u, 1u, 1u, {3u, 5u, 13u, 3u}, {24, 17, 12, 17}, 4u, 768u},
    {24u, 48u, 5u, 1u, {4u, 3u, 26u, 3u}, {5, 4, 2, 3}, 0u, 1152u},
    {29u, 48u, 4u, 1u, {3u, 4u, 26u, 3u}, {9, 6, 4, 6}, 0u, 1152u},
    {35u, 48u, 3u, 1u, {3u, 4u, 26u, 3u}, {15, 10, 6, 9}, 4u, 1152u},
    {42u, 48u, 2u, 1u, {3u, 4u, 26u, 3u}, {24, 14, 8, 15}, 0u, 1152u},
    {52u, 48u, 1u, 1u, {3u, 5u, 25u, 3u}, {24, 18, 13, 18}, 0u, 1152u},
    {29u, 56u, 5u, 1u, {6u, 10u, 23u, 3u}, {5, 4, 2, 3}, 0u, 1344u},
    {35u, 56u, 4u, 1u, {6u, 10u, 23u, 3u}, {9, 6, 4, 5}, 0u, 1344u},
    {42u, 56u, 3u, 1u, {6u, 12u, 21u, 3u}, {16, 7, 6, 9}, 0u, 1344u},
    {52u, 56u, 2u, 1u, {6u, 10u, 23u, 3u}, {23, 13, 8, 13}, 8u, 1344u},
    {32u, 64u, 5u, 1u, {6u, 9u, 31u, 2u}, {5, 3, 2, 3}, 0u, 1536u},
    {42u, 64u, 4u, 1u, {6u, 9u, 33u, 0u}, {11, 6, 5, -1}, 0u, 1536u},
    {48u, 64u, 3u, 1u, {6u, 12u, 27u, 3u}, {16, 8, 6, 9}, 0u, 1536u},
    {58u, 64u, 2u, 1u, {6u, 10u, 29u, 3u}, {23, 13, 8, 13}, 8u, 1536u},
    {70u, 64u, 1u, 1u, {6u, 11u, 28u, 3u}, {24, 18, 12, 18}, 4u, 1536u},
    {40u, 80u, 5u, 1u, {6u, 10u, 41u, 3u}, {6, 3, 2, 3}, 0u, 1920u},
    {52u, 80u, 4u, 1u, {6u, 10u, 41u, 3u}, {11, 6, 5, 6}, 0u, 1920u},
    {58u, 80u, 3u, 1u, {6u, 11u, 40u, 3u}, {16, 8, 6, 7}, 0u, 1920u},
    {70u, 80u, 2u, 1u, {6u, 10u, 41u, 3u}, {23, 13, 8, 13}, 8u, 1920u},
    {84u, 80u, 1u, 1u, {6u, 10u, 41u, 3u}, {24, 17, 12, 18}, 4u, 1920u},
    {48u, 96u, 5u, 1u, {7u, 9u, 53u, 3u}, {5, 4, 2, 4}, 0u, 2304u},
    {58u, 96u, 4u, 1u, {7u, 10u, 52u, 3u}, {9, 6, 4, 6}, 0u, 2304u},
    {70u, 96u, 3u, 1u, {6u, 12u, 51u, 3u}, {16, 9, 6, 10}, 4u, 2304u},
    {84u, 96u, 2u, 1u, {6u, 10u, 53u, 3u}, {22, 12, 9, 12}, 0u, 2304u},
    {104u, 96u, 1u, 1u, {6u, 13u, 50u, 3u}, {24, 18, 13, 19}, 0u, 2304u},
    {58u, 112u, 5u, 1u, {14u, 17u, 50u, 3u}, {5, 4, 2, 5}, 0u, 2688u},
    {70u, 112u, 4u, 1u, {11u, 21u, 49u, 3u}, {9, 6, 4, 8}, 0u, 2688u},
    {84u, 112u, 3u, 1u, {11u, 23u, 47u, 3u}, {16, 8, 6, 9}, 0u, 2688u},
    {104u, 112u, 2u, 1u, {11u, 21u, 49u, 3u}, {23, 12, 9, 14}, 4u, 2688u},
    {64u, 128u, 5u, 1u, {12u, 19u, 62u, 3u}, {5, 3, 2, 4}, 0u, 3072u},
    {84u, 128u, 4u, 1u, {11u, 21u, 61u, 3u}, {11, 6, 5, 7}, 0u, 3072u},
    {96u, 128u, 3u, 1u, {11u, 22u, 60u, 3u}, {16, 9, 6, 10}, 4u, 3072u},
    {116u, 128u, 2u, 1u, {11u, 21u, 61u, 3u}, {22, 12, 9, 14}, 0u, 3072u},
    {140u, 128u, 1u, 1u, {11u, 20u, 62u, 3u}, {24, 17, 13, 19}, 8u, 3072u},
    {80u, 160u, 5u, 1u, {11u, 19u, 87u, 3u}, {5, 4, 2, 4}, 0u, 3840u},
    {104u, 160u, 4u, 1u, {11u, 23u, 83u, 3u}, {11, 6, 5, 9}, 0u, 3840u},
    {116u, 160u, 3u, 1u, {11u, 24u, 82u, 3u}, {16, 8, 6, 11}, 0u, 3840u},
    {140u, 160u, 2u, 1u, {11u, 21u, 85u, 3u}, {22, 11, 9, 13}, 0u, 3840u},
    {168u, 160u, 1u, 1u, {11u, 22u, 84u, 3u}, {24, 18, 12, 19}, 0u, 3840u},
    {96u, 192u, 5u, 1u, {11u, 20u, 110u, 3u}, {6, 4, 2, 5}, 0u, 4608u},
    {116u, 192u, 4u, 1u, {11u, 22u, 108u, 3u}, {10, 6, 4, 9}, 0u, 4608u},
    {140u, 192u, 3u, 1u, {11u, 24u, 106u, 3u}, {10, 6, 4, 9}, 0u, 4608u},
    {168u, 192u, 2u, 1u, {11u, 20u, 110u, 3u}, {22, 13, 9, 13}, 8u, 4608u},
    {208u, 192u, 1u, 1u, {11u, 21u, 109u, 3u}, {24, 20, 13, 24}, 0u, 4608u},
    {116u, 224u, 5u, 1u, {12u, 22u, 131u, 3u}, {8, 6, 2, 6}, 4u, 5376u},
    {140u, 224u, 4u, 1u, {12u, 26u, 127u, 3u}, {12, 8, 4, 11}, 0u, 5376u},
    {168u, 224u, 3u, 1u, {11u, 20u, 134u, 3u}, {16, 10, 7, 9}, 0u, 5376u},
    {208u, 224u, 2u, 1u, {11u, 22u, 132u, 3u}, {24, 16, 10, 15}, 0u, 5376u},
    {232u, 224u, 1u, 1u, {11u, 24u, 130u, 3u}, {24, 20, 12, 20}, 4u, 5376u},
    {128u, 256u, 5u, 1u, {11u, 24u, 154u, 3u}, {6, 5, 2, 5}, 0u, 6144u},
    {168u, 256u, 4u, 1u, {11u, 24u, 154u, 3u}, {12, 9, 5, 10}, 4u, 6144u},
    {192u, 256u, 3u, 1u, {11u, 27u, 151u, 3u}, {16, 10, 7, 10}, 0u, 6144u},
    {232u, 256u, 2u, 1u, {11u, 22u, 156u, 3u}, {24, 14, 10, 13}, 8u, 6144u},
    {280u, 256u, 1u, 1u, {11u, 26u, 152u, 3u}, {24, 19, 14, 18}, 4u, 6144u},
    {160u, 320u, 5u, 1u, {11u, 26u, 200u, 3u}, {8, 5, 2, 6}, 4u, 7680u},
    {208u, 320u, 4u, 1u, {11u, 25u, 201u, 3u}, {13, 9, 5, 10}, 8u, 7680u},
    {280u, 320u, 2u, 1u, {11u, 26u, 200u, 3u}, {24, 17, 9, 17}, 0u, 7680u},
    {192u, 384u, 5u, 1u, {11u, 27u, 247u, 3u}, {8, 6, 2, 7}, 0u, 9216u},
    {280u, 384u, 3u, 1u, {11u, 24u, 250u, 3u}, {16, 9, 7, 10}, 4u, 9216u},
    {416u, 384u, 1u, 1u, {12u, 28u, 245u, 3u}, {24, 20, 14, 23}, 8u, 9216u}
};

static const uint8_t DAB_MSC_DEINTERLEAVER_DELAYS[DAB_MSC_CONV_DEINTERLEAVER_DEPTH] = {
    15u, 7u, 11u, 3u, 13u, 5u, 9u, 1u, 14u, 6u, 10u, 2u, 12u, 4u, 8u, 0u
};

static uint8_t dab_msc_rs_gf_initialized = 0u;
static uint8_t dab_msc_rs_gf_log[256];
static uint8_t dab_msc_rs_gf_binary[256];
static uint32_t dab_msc_dabplus_debug_reject_logs = 0u;

static void dab_msc_rs_gf_init(void) {
    uint16_t r = 1u;
    uint32_t i;

    if (dab_msc_rs_gf_initialized != 0u) {
        return;
    }

    memset(dab_msc_rs_gf_log, 0, sizeof(dab_msc_rs_gf_log));
    memset(dab_msc_rs_gf_binary, 0, sizeof(dab_msc_rs_gf_binary));
    for (i = 0u; i < 255u; ++i) {
        dab_msc_rs_gf_log[r] = (uint8_t)i;
        dab_msc_rs_gf_binary[i] = (uint8_t)r;
        r <<= 1u;
        if (r >= 256u) {
            r &= 0xFFu;
            r ^= (1u << 4u) | (1u << 3u) | (1u << 2u) | 1u;
        }
    }
    dab_msc_rs_gf_initialized = 1u;
}

static uint8_t dab_msc_rs_gf_mul(uint8_t a, uint8_t b) {
    if (a == 0u || b == 0u) {
        return 0u;
    }
    dab_msc_rs_gf_init();
    return dab_msc_rs_gf_binary[(dab_msc_rs_gf_log[a] + dab_msc_rs_gf_log[b]) % 255u];
}

static uint8_t dab_msc_rs_gf_inv(uint8_t a) {
    if (a == 0u) {
        return 0u;
    }
    dab_msc_rs_gf_init();
    return dab_msc_rs_gf_binary[255u - dab_msc_rs_gf_log[a]];
}

static uint8_t dab_msc_rs_gf_binary_value(uint8_t power) {
    dab_msc_rs_gf_init();
    return dab_msc_rs_gf_binary[power % 255u];
}

static uint16_t dab_msc_dabplus_firecode(const uint8_t *data) {
    uint16_t z = 0u;
    uint32_t i;

    for (i = 0u; i < 9u; ++i) {
        uint32_t b;

        for (b = 0u; b < 8u; ++b) {
            const uint8_t c = (uint8_t)(((z >> 15u) & 1u) ^ ((data[i] >> (7u - b)) & 1u));

            z = (uint16_t)((z << 1u) & 0xFFFFu);
            if (c != 0u) {
                z ^= 0x782Fu;
            }
        }
    }

    return z;
}

static void dab_msc_rs_syndromes_245_10(uint8_t a[255], uint8_t b[10]) {
    static const uint8_t roots[10] = {1u, 2u, 4u, 8u, 16u, 32u, 64u, 128u, 29u, 58u};
    uint32_t i;

    memset(b, 0, 10u);
    for (i = 0u; i < 10u; ++i) {
        uint8_t r = 1u;
        uint32_t j;

        for (j = 0u; j < 255u; ++j) {
            b[i] ^= dab_msc_rs_gf_mul(r, a[254u - j]);
            r = dab_msc_rs_gf_mul(r, roots[i]);
        }
    }
}

static int dab_msc_rs_chien_245_10(uint8_t l[10], uint8_t x[10]) {
    static const uint8_t roots[10] = {1u, 2u, 4u, 8u, 16u, 32u, 64u, 128u, 29u, 58u};
    uint8_t lambda[10];
    uint32_t k = 0u;
    uint32_t j;

    memcpy(lambda, l, sizeof(lambda));
    {
        uint8_t a = 0u;

        for (j = 0u; j < 10u; ++j) {
            a ^= lambda[j];
        }
        if (a == 0u && k < 10u) {
            x[k++] = 0u;
        }
    }

    for (uint32_t i = 1u; i < 255u; ++i) {
        uint8_t a = 0u;

        for (j = 0u; j < 10u; ++j) {
            lambda[j] = dab_msc_rs_gf_mul(lambda[j], roots[j]);
            a ^= lambda[j];
        }
        if (a == 0u && k < 10u) {
            x[k++] = (uint8_t)(255u - i);
        }
    }

    return (int)k;
}

static int dab_msc_rs_berlekamp_massey_245_10(uint8_t s[10], uint8_t r[10]) {
    uint32_t l = 1u;
    uint32_t dm = 1u;
    uint32_t L = 0u;
    uint8_t c[11] = {0};
    uint8_t p[11] = {0};

    c[0] = 1u;
    p[0] = 1u;
    for (uint32_t k = 0u; k < 10u; ++k) {
        uint8_t d = s[k];

        for (uint32_t i = 1u; i <= L; ++i) {
            d ^= dab_msc_rs_gf_mul(c[i], s[k - i]);
        }
        if (d == 0u) {
            l++;
        } else if ((2u * L) >= (k + 1u)) {
            const uint8_t ddm = dab_msc_rs_gf_mul(d, dab_msc_rs_gf_inv((uint8_t)dm));

            for (uint32_t i = l; i < 10u; ++i) {
                c[i] ^= dab_msc_rs_gf_mul(ddm, p[i - l]);
            }
            l++;
        } else {
            uint8_t t[10] = {0};
            const uint8_t inv = dab_msc_rs_gf_inv((uint8_t)dm);
            const uint8_t ddm = dab_msc_rs_gf_mul(d, inv);

            memcpy(t, c, sizeof(t));
            for (uint32_t i = l; i < 10u; ++i) {
                c[i] ^= dab_msc_rs_gf_mul(ddm, p[i - l]);
            }
            L = k + 1u - L;
            memset(p, 0, sizeof(p));
            memcpy(p, t, sizeof(t));
            dm = d;
            l = 1u;
        }
    }

    memcpy(r, c, 10u);
    return (int)L;
}

static void dab_msc_rs_forney_245_10(uint8_t s[10], uint8_t l[10], uint8_t x[10], int xlen, uint8_t e[10]) {
    uint8_t z[10] = {0};
    uint8_t f[10] = {0};

    for (int i = 9; i >= 1; --i) {
        z[9] = dab_msc_rs_gf_mul(s[9], l[i]) ^ z[8];
        z[8] = dab_msc_rs_gf_mul(s[8], l[i]) ^ z[7];
        z[7] = dab_msc_rs_gf_mul(s[7], l[i]) ^ z[6];
        z[6] = dab_msc_rs_gf_mul(s[6], l[i]) ^ z[5];
        z[5] = dab_msc_rs_gf_mul(s[5], l[i]) ^ z[4];
        z[4] = dab_msc_rs_gf_mul(s[4], l[i]) ^ z[3];
        z[3] = dab_msc_rs_gf_mul(s[3], l[i]) ^ z[2];
        z[2] = dab_msc_rs_gf_mul(s[2], l[i]) ^ z[1];
        z[1] = dab_msc_rs_gf_mul(s[1], l[i]) ^ z[0];
        z[0] = dab_msc_rs_gf_mul(s[0], l[i]);
    }

    z[9] = s[9] ^ z[8];
    z[8] = s[8] ^ z[7];
    z[7] = s[7] ^ z[6];
    z[6] = s[6] ^ z[5];
    z[5] = s[5] ^ z[4];
    z[4] = s[4] ^ z[3];
    z[3] = s[3] ^ z[2];
    z[2] = s[2] ^ z[1];
    z[1] = s[1] ^ z[0];
    z[0] = s[0];

    f[0] = l[0];
    f[2] = l[2];
    f[4] = l[4];
    f[6] = l[6];
    f[8] = l[8];

    memset(e, 0, 10u);
    for (int i = 0; i < xlen; ++i) {
        uint8_t r = 1u;
        uint8_t c = 0u;
        uint8_t m = 0u;

        for (uint8_t j = 0u; j < 10u; ++j) {
            c ^= dab_msc_rs_gf_mul(z[j], r);
            m ^= dab_msc_rs_gf_mul(f[j], r);
            r = dab_msc_rs_gf_mul(r, dab_msc_rs_gf_binary_value((uint8_t)(255u - x[i])));
        }
        e[i] = dab_msc_rs_gf_mul(c, dab_msc_rs_gf_inv(m));
    }
}

static uint32_t dab_msc_rs_decode_255_245_10(uint8_t a[255]) {
    uint8_t s[10] = {0};
    uint8_t x[10] = {0};
    uint8_t l[10] = {0};
    uint8_t e[10] = {0};
    int L;
    int xlen;
    uint32_t syndrome_nonzero = 0u;

    dab_msc_rs_syndromes_245_10(a, s);
    for (uint32_t i = 0u; i < 10u; ++i) {
        if (s[i] != 0u) {
            syndrome_nonzero = 1u;
            break;
        }
    }
    if (syndrome_nonzero == 0u) {
        return 0u;
    }
    L = dab_msc_rs_berlekamp_massey_245_10(s, l);
    xlen = dab_msc_rs_chien_245_10(l, x);
    dab_msc_rs_forney_245_10(s, l, x, xlen, e);
    if (xlen == L) {
        for (int i = 0; i < xlen; ++i) {
            a[254 - x[i]] ^= e[i];
        }
        return (uint32_t)xlen;
    }
    return 0u;
}

static uint32_t dab_msc_rs_decode_120_110(uint8_t row[120]) {
    uint8_t b[255] = {0};

    memcpy(b + 135u, row, 120u);
    {
        const uint32_t corrected = dab_msc_rs_decode_255_245_10(b);
        memcpy(row, b + 135u, 120u);
        return corrected;
    }
}

static void dab_msc_free_dabplus_stream(dab_msc_dabplus_stream_runtime_t *runtime) {
    if (runtime == 0) {
        return;
    }
    free(runtime->columns);
    free(runtime->superframe);
    free(runtime);
}

static void dab_msc_reset_dabplus_stream(dab_msc_dabplus_stream_runtime_t *runtime) {
    if (runtime == 0) {
        return;
    }
    runtime->columns_filled = 0u;
    runtime->input_row = 0u;
    runtime->synchronized = 0u;
    runtime->superframe_valid = 0u;
    runtime->generation = 0u;
    runtime->dac_rate = 0u;
    runtime->sbr_flag = 0u;
    runtime->aac_channel_mode = 0u;
    runtime->ps_flag = 0u;
    runtime->mpeg_surround_config = 0u;
    runtime->au_count = 0u;
    runtime->superframe_bytes = 0u;
    runtime->decode_attempts = 0u;
    runtime->decode_failures = 0u;
    runtime->rs_corrected_last_superframe = 0u;
    runtime->rs_corrected_total = 0u;
    runtime->last_decode_ok = 0u;
    memset(runtime->au_start, 0, sizeof(runtime->au_start));
    memset(runtime->au_length, 0, sizeof(runtime->au_length));
    if (runtime->columns != 0) {
        memset(runtime->columns,
               0,
               (size_t)runtime->row_count * DAB_MSC_DABPLUS_COLUMNS_PER_SUPERFRAME);
    }
    if (runtime->superframe != 0) {
        memset(runtime->superframe,
               0,
               (size_t)runtime->row_count * DAB_MSC_DABPLUS_COLUMNS_PER_RS_PAYLOAD);
    }
}

static dab_msc_dabplus_stream_runtime_t *dab_msc_ensure_dabplus_stream(dab_msc_subchannel_runtime_t *runtime,
                                                                       uint16_t cif_bytes) {
    dab_msc_dabplus_stream_runtime_t *dabplus;

    if (runtime == 0 || cif_bytes == 0u || (cif_bytes % DAB_MSC_DABPLUS_RESYNC_COLUMNS) != 0u) {
        return 0;
    }

    dabplus = runtime->dabplus;
    if (dabplus != 0 && dabplus->cif_bytes == cif_bytes) {
        return dabplus;
    }

    dab_msc_free_dabplus_stream(dabplus);
    dabplus = (dab_msc_dabplus_stream_runtime_t *)calloc(1u, sizeof(*dabplus));
    if (dabplus == 0) {
        runtime->dabplus = 0;
        return 0;
    }
    dabplus->cif_bytes = cif_bytes;
    dabplus->row_count = (uint16_t)(cif_bytes / DAB_MSC_DABPLUS_RESYNC_COLUMNS);
    dabplus->columns = (uint8_t *)calloc((size_t)dabplus->row_count, DAB_MSC_DABPLUS_COLUMNS_PER_SUPERFRAME);
    dabplus->superframe = (uint8_t *)calloc((size_t)dabplus->row_count, DAB_MSC_DABPLUS_COLUMNS_PER_RS_PAYLOAD);
    if (dabplus->columns == 0 || dabplus->superframe == 0) {
        dab_msc_free_dabplus_stream(dabplus);
        runtime->dabplus = 0;
        return 0;
    }
    runtime->dabplus = dabplus;
    dab_msc_reset_dabplus_stream(dabplus);
    return dabplus;
}

static int dab_msc_parse_dabplus_superframe(dab_msc_dabplus_stream_runtime_t *runtime,
                                            const uint8_t *header_bytes) {
    static const uint8_t first_au_start[7] = {0u, 0u, 5u, 6u, 8u, 0u, 11u};
    uint32_t au_count;
    uint32_t limit;

    if (runtime == 0 || header_bytes == 0 || runtime->superframe == 0 || runtime->superframe_bytes < 3u) {
        return -1;
    }

    runtime->dac_rate = (uint8_t)((runtime->superframe[2] >> 6u) & 1u);
    runtime->sbr_flag = (uint8_t)((runtime->superframe[2] >> 5u) & 1u);
    runtime->aac_channel_mode = (uint8_t)((runtime->superframe[2] >> 4u) & 1u);
    runtime->ps_flag = (uint8_t)((runtime->superframe[2] >> 3u) & 1u);
    runtime->mpeg_surround_config = (uint8_t)(runtime->superframe[2] & 0x07u);

    if (runtime->dac_rate == 0u && runtime->sbr_flag != 0u) {
        au_count = 2u;
    } else if (runtime->dac_rate != 0u && runtime->sbr_flag != 0u) {
        au_count = 3u;
    } else if (runtime->dac_rate == 0u && runtime->sbr_flag == 0u) {
        au_count = 4u;
    } else {
        au_count = 6u;
    }
    if (au_count >= sizeof(first_au_start) || au_count > DAB_MSC_DABPLUS_MAX_AUS) {
        return -1;
    }

    memset(runtime->au_start, 0, sizeof(runtime->au_start));
    memset(runtime->au_length, 0, sizeof(runtime->au_length));
    runtime->au_count = (uint8_t)au_count;
    runtime->au_start[0] = first_au_start[au_count];
    runtime->au_start[1] = ((uint32_t)header_bytes[3] << 4u) | ((uint32_t)header_bytes[4] >> 4u);
    if (au_count >= 3u) {
        runtime->au_start[2] = (((uint32_t)header_bytes[4] & 0x0Fu) << 8u) | (uint32_t)header_bytes[5];
    }
    if (au_count >= 4u) {
        runtime->au_start[3] = ((uint32_t)header_bytes[6] << 4u) | ((uint32_t)header_bytes[7] >> 4u);
    }
    if (au_count == 6u) {
        runtime->au_start[4] = (((uint32_t)header_bytes[7] & 0x0Fu) << 8u) | (uint32_t)header_bytes[8];
        runtime->au_start[5] = ((uint32_t)header_bytes[9] << 4u) | ((uint32_t)header_bytes[10] >> 4u);
    }

    limit = runtime->superframe_bytes;
    for (uint32_t i = 0u; i < au_count; ++i) {
        const uint32_t next = (i + 1u < au_count) ? runtime->au_start[i + 1u] : limit;

        if (runtime->au_start[i] >= next || next > limit || (next - runtime->au_start[i]) < 2u) {
            runtime->au_count = 0u;
            return -1;
        }
        runtime->au_length[i] = next - runtime->au_start[i] - 2u;
    }

    return 0;
}

static int dab_msc_dabplus_decode_superframe(dab_msc_dabplus_stream_runtime_t *runtime) {
    uint8_t *header;
    uint8_t *raw_superframe;
    uint8_t row[120];
    uint32_t row_index;
    uint32_t out_pos = 0u;
    uint8_t raw_dac_rate = 0u;
    uint8_t raw_sbr_flag = 0u;
    uint8_t raw_aac_channel_mode = 0u;
    uint8_t raw_ps_flag = 0u;
    uint8_t raw_mpeg_surround_config = 0u;
    uint8_t raw_au_count = 0u;
    uint32_t raw_au_start[DAB_MSC_DABPLUS_MAX_AUS] = {0};
    uint32_t raw_au_length[DAB_MSC_DABPLUS_MAX_AUS] = {0};
    uint16_t firecode_expected;
    uint16_t firecode_actual;
    int header_zero;
    int raw_parse_ok;
    uint32_t rs_corrected = 0u;

    if (runtime == 0 || runtime->columns == 0 || runtime->superframe == 0 ||
        runtime->columns_filled != DAB_MSC_DABPLUS_COLUMNS_PER_SUPERFRAME) {
        return -1;
    }

    header = (uint8_t *)malloc((size_t)runtime->row_count * 2u);
    if (header == 0) {
        return -1;
    }
    raw_superframe = (uint8_t *)malloc((size_t)runtime->row_count * DAB_MSC_DABPLUS_COLUMNS_PER_RS_PAYLOAD);
    if (raw_superframe == 0) {
        free(header);
        return -1;
    }
    for (uint32_t col = 0u; col < 2u; ++col) {
        for (row_index = 0u; row_index < runtime->row_count; ++row_index) {
            header[(col * runtime->row_count) + row_index] =
                runtime->columns[(row_index * DAB_MSC_DABPLUS_COLUMNS_PER_SUPERFRAME) + col];
        }
    }

    firecode_expected = (uint16_t)(((uint16_t)header[0] << 8u) | (uint16_t)header[1]);
    firecode_actual = dab_msc_dabplus_firecode(header + 2u);
    header_zero = (header[3] == 0u && header[4] == 0u);
    if (firecode_expected != firecode_actual || header_zero) {
        if (dab_msc_dabplus_debug_reject_logs < 8u) {
            fprintf(stderr,
                    "dabplus reject firecode_expected=0x%04x firecode_actual=0x%04x header_zero=%d header=%02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x\n",
                    firecode_expected,
                    firecode_actual,
                    header_zero,
                    header[0],
                    header[1],
                    header[2],
                    header[3],
                    header[4],
                    header[5],
                    header[6],
                    header[7],
                    header[8],
                    header[9],
                    header[10]);
            dab_msc_dabplus_debug_reject_logs++;
        }
        free(header);
        free(raw_superframe);
        runtime->synchronized = 0u;
        runtime->superframe_valid = 0u;
        return -1;
    }

    runtime->superframe_bytes = (uint32_t)runtime->row_count * DAB_MSC_DABPLUS_COLUMNS_PER_RS_PAYLOAD;
    for (uint32_t col = 0u; col < DAB_MSC_DABPLUS_COLUMNS_PER_RS_PAYLOAD; ++col) {
        for (row_index = 0u; row_index < runtime->row_count; ++row_index) {
            raw_superframe[(col * runtime->row_count) + row_index] =
                runtime->columns[(row_index * DAB_MSC_DABPLUS_COLUMNS_PER_SUPERFRAME) + col];
        }
    }
    memcpy(runtime->superframe, raw_superframe, runtime->superframe_bytes);
    raw_parse_ok = dab_msc_parse_dabplus_superframe(runtime, runtime->superframe) == 0;
    if (raw_parse_ok) {
        raw_dac_rate = runtime->dac_rate;
        raw_sbr_flag = runtime->sbr_flag;
        raw_aac_channel_mode = runtime->aac_channel_mode;
        raw_ps_flag = runtime->ps_flag;
        raw_mpeg_surround_config = runtime->mpeg_surround_config;
        raw_au_count = runtime->au_count;
        memcpy(raw_au_start, runtime->au_start, sizeof(raw_au_start));
        memcpy(raw_au_length, runtime->au_length, sizeof(raw_au_length));
    }

    for (row_index = 0u; row_index < runtime->row_count; ++row_index) {
        for (uint32_t col = 0u; col < DAB_MSC_DABPLUS_COLUMNS_PER_SUPERFRAME; ++col) {
            row[col] = runtime->columns[(row_index * DAB_MSC_DABPLUS_COLUMNS_PER_SUPERFRAME) + col];
        }
        rs_corrected += dab_msc_rs_decode_120_110(row);
        memcpy(runtime->columns + (row_index * DAB_MSC_DABPLUS_COLUMNS_PER_SUPERFRAME),
               row,
               sizeof(row));
    }
    for (uint32_t col = 0u; col < DAB_MSC_DABPLUS_COLUMNS_PER_RS_PAYLOAD; ++col) {
        for (row_index = 0u; row_index < runtime->row_count; ++row_index) {
            runtime->superframe[out_pos++] =
                runtime->columns[(row_index * DAB_MSC_DABPLUS_COLUMNS_PER_SUPERFRAME) + col];
        }
    }

    runtime->synchronized = 1u;
    runtime->rs_corrected_last_superframe = rs_corrected;
    runtime->rs_corrected_total += rs_corrected;
    runtime->superframe_valid = dab_msc_parse_dabplus_superframe(runtime, runtime->superframe) == 0 ? 1u : 0u;
    if (runtime->superframe_valid == 0u && raw_parse_ok) {
        memcpy(runtime->superframe, raw_superframe, runtime->superframe_bytes);
        runtime->dac_rate = raw_dac_rate;
        runtime->sbr_flag = raw_sbr_flag;
        runtime->aac_channel_mode = raw_aac_channel_mode;
        runtime->ps_flag = raw_ps_flag;
        runtime->mpeg_surround_config = raw_mpeg_surround_config;
        runtime->au_count = raw_au_count;
        memcpy(runtime->au_start, raw_au_start, sizeof(runtime->au_start));
        memcpy(runtime->au_length, raw_au_length, sizeof(runtime->au_length));
        runtime->superframe_valid = 1u;
    }
    if (runtime->superframe_valid != 0u) {
        runtime->generation++;
    }
    free(header);
    free(raw_superframe);
    return runtime->superframe_valid != 0u ? 0 : -1;
}

static int dab_msc_dabplus_feed_cif(dab_msc_dabplus_stream_runtime_t *runtime,
                                    const uint8_t *data,
                                    uint16_t byte_count) {
    uint32_t q;

    if (runtime == 0 || data == 0 || byte_count == 0u || runtime->cif_bytes != byte_count ||
        runtime->columns == 0 || runtime->row_count == 0u) {
        return -1;
    }

    if (runtime->columns_filled >= DAB_MSC_DABPLUS_COLUMNS_PER_SUPERFRAME) {
        runtime->columns_filled = 0u;
        runtime->input_row = 0u;
    }

    for (q = 0u; q < byte_count; ++q) {
        runtime->columns[(runtime->input_row * DAB_MSC_DABPLUS_COLUMNS_PER_SUPERFRAME) + runtime->columns_filled] =
            data[q];
        runtime->input_row++;
        if (runtime->input_row == runtime->row_count) {
            runtime->input_row = 0u;
            runtime->columns_filled++;
            if (runtime->columns_filled == DAB_MSC_DABPLUS_COLUMNS_PER_SUPERFRAME) {
                break;
            }
        }
    }
    if (runtime->columns_filled < DAB_MSC_DABPLUS_COLUMNS_PER_SUPERFRAME) {
        return 0;
    }

    runtime->decode_attempts++;
    if (dab_msc_dabplus_decode_superframe(runtime) == 0) {
        runtime->last_decode_ok = 1u;
        runtime->columns_filled = 0u;
        return 0;
    }
    runtime->decode_failures++;
    runtime->last_decode_ok = 0u;

    for (uint32_t row_index = 0u; row_index < runtime->row_count; ++row_index) {
        memmove(runtime->columns + (row_index * DAB_MSC_DABPLUS_COLUMNS_PER_SUPERFRAME),
                runtime->columns + (row_index * DAB_MSC_DABPLUS_COLUMNS_PER_SUPERFRAME) + DAB_MSC_DABPLUS_RESYNC_COLUMNS,
                DAB_MSC_DABPLUS_COLUMNS_PER_SUPERFRAME - DAB_MSC_DABPLUS_RESYNC_COLUMNS);
        memset(runtime->columns + (row_index * DAB_MSC_DABPLUS_COLUMNS_PER_SUPERFRAME) +
                   (DAB_MSC_DABPLUS_COLUMNS_PER_SUPERFRAME - DAB_MSC_DABPLUS_RESYNC_COLUMNS),
               0,
               DAB_MSC_DABPLUS_RESYNC_COLUMNS);
    }
    runtime->columns_filled = DAB_MSC_DABPLUS_COLUMNS_PER_SUPERFRAME - DAB_MSC_DABPLUS_RESYNC_COLUMNS;
    runtime->input_row = 0u;
    return 0;
}

static uint8_t dab_msc_prbs_bit(int reset) {
    static uint16_t reg = 0x1FFu;
    uint8_t o;
    uint8_t o5;
    uint8_t s;

    if (reset) {
        reg = 0x1FFu;
    }

    o = (uint8_t)((reg & 0x100u) != 0u);
    o5 = (uint8_t)((reg & 0x010u) != 0u);
    s = (uint8_t)((o ^ o5) & 1u);
    reg = (uint16_t)(((reg << 1u) & 0x1FFu) | s);
    return s;
}

static uint8_t dab_msc_prbs81(uint32_t reset) {
    uint8_t value = 0u;
    uint32_t i;

    for (i = 0u; i < 8u; ++i) {
        value = (uint8_t)((value << 1u) | dab_msc_prbs_bit(reset != 0u && i == 0u));
    }

    return value;
}

static uint32_t dab_msc_puncturing_code(uint8_t index) {
    static const uint32_t table[26] = {
        0u,
        0x11111113u, 0x11131113u, 0x11131313u, 0x13131313u, 0x13131333u,
        0x13331333u, 0x13333333u, 0x33333333u, 0x73333333u, 0x73337333u,
        0x73737333u, 0x73737373u, 0x37373777u, 0x37773777u, 0x37777777u,
        0x77777777u, 0xF7777777u, 0xF777F777u, 0xF7F7F777u, 0xF7F7F7F7u,
        0xFFF7F7F7u, 0xFFF7FFF7u, 0xFFFFFFF7u, 0xFFFFFFFFu, 0x00333333u
    };

    return index < 26u ? table[index] : 0u;
}

static uint8_t dab_msc_bitcount32(uint32_t value) {
    uint8_t count = 0u;

    while (value != 0u) {
        count = (uint8_t)(count + (value & 1u));
        value >>= 1u;
    }
    return count;
}

static int dab_msc_append_segment(dab_msc_decoder_config_t *config, uint32_t input_bits, uint8_t code_index) {
    dab_msc_puncture_segment_t *segment;
    uint32_t code;

    if (config == 0 || config->segment_count >= DAB_MSC_MAX_PUNCTURE_SEGMENTS) {
        return -1;
    }

    code = dab_msc_puncturing_code(code_index);
    if (code == 0u && code_index != 0u) {
        return -1;
    }

    segment = &config->segments[config->segment_count++];
    segment->code_index = code_index;
    segment->code_bits = dab_msc_bitcount32(code);
    segment->input_bits = input_bits;
    if (input_bits == 24u) {
        config->conv_len += segment->code_bits;
    } else {
        config->conv_len += (input_bits / 32u) * segment->code_bits;
    }
    return 0;
}

static int dab_msc_eep_bitrate(uint8_t option, uint8_t protection_level, uint16_t size_cu, uint16_t *bitrate_kbps) {
    static const uint8_t set_a_cu_per_n[] = {12u, 8u, 6u, 4u};
    static const uint8_t set_b_cu_per_n[] = {27u, 21u, 18u, 15u};
    const uint8_t *cu_per_n;
    uint32_t unit_rate_kbps;

    if (bitrate_kbps == 0 || protection_level >= 4u) {
        return -1;
    }

    if (option == 0u) {
        cu_per_n = set_a_cu_per_n;
        unit_rate_kbps = 8u;
    } else if (option == 1u) {
        cu_per_n = set_b_cu_per_n;
        unit_rate_kbps = 32u;
    } else {
        return -1;
    }

    if (size_cu == 0u || (size_cu % cu_per_n[protection_level]) != 0u) {
        return -1;
    }

    *bitrate_kbps = (uint16_t)(((uint32_t)size_cu / cu_per_n[protection_level]) * unit_rate_kbps);
    return 0;
}

static int dab_msc_resolve_subchannel(const dab_fic_subchannel_t *in, dab_msc_subchannel_t *out) {
    if (in == 0 || out == 0 || in->valid == 0u) {
        return -1;
    }

    *out = (dab_msc_subchannel_t){0};
    out->valid = 1u;
    out->subchid = in->subchid;
    out->start_cu = in->start_cu;
    out->protection_index = in->protection_index;
    out->protection_option = in->protection_option;
    out->cif_count = DAB_MSC_CIFS_PER_FRAME;

    if (in->protection_kind == DAB_FIC_PROTECTION_UEP) {
        const dab_msc_uep_entry_t *entry;

        if (in->table_switch != 0u || in->protection_index >= (uint8_t)(sizeof(DAB_MSC_UEP_TABLE) / sizeof(DAB_MSC_UEP_TABLE[0]))) {
            return -1;
        }
        entry = &DAB_MSC_UEP_TABLE[in->protection_index];
        if (entry->valid == 0u) {
            return -1;
        }
        out->profile = DAB_MSC_PROFILE_UEP;
        out->size_cu = entry->size_cu;
        out->bitrate_kbps = entry->bitrate_kbps;
        out->protection_level = entry->protection_level;
    } else {
        uint16_t bitrate_kbps = 0u;

        if (dab_msc_eep_bitrate(in->protection_option, in->protection_level, in->size_cu, &bitrate_kbps) != 0) {
            return -1;
        }
        out->profile = in->protection_option == 0u ? DAB_MSC_PROFILE_EEP_A : DAB_MSC_PROFILE_EEP_B;
        out->size_cu = in->size_cu;
        out->bitrate_kbps = bitrate_kbps;
        out->protection_level = (uint8_t)(in->protection_level + 1u);
    }

    if (out->size_cu == 0u || out->start_cu >= DAB_MSC_CUS_PER_CIF ||
        ((uint32_t)out->start_cu + out->size_cu) > DAB_MSC_CUS_PER_CIF) {
        return -1;
    }

    out->softbits_per_cif = (uint32_t)out->size_cu * DAB_MSC_SOFTBITS_PER_CU;
    return 0;
}

static int dab_msc_fill_subchannel_frame(const dab_msc_subchannel_t *subchannel,
                                         uint32_t msc_softbit_count,
                                         dab_msc_subchannel_frame_t *frame) {
    uint32_t cif_index;

    if (subchannel == 0 || frame == 0 || subchannel->valid == 0u || msc_softbit_count != DAB_TFRAME_MSC_SOFTBITS) {
        return -1;
    }

    *frame = (dab_msc_subchannel_frame_t){0};
    frame->valid = 1u;
    frame->subchid = subchannel->subchid;
    frame->start_cu = subchannel->start_cu;
    frame->size_cu = subchannel->size_cu;
    frame->bitrate_kbps = subchannel->bitrate_kbps;
    frame->profile = subchannel->profile;
    frame->protection_level = subchannel->protection_level;
    frame->cif_count = DAB_MSC_CIFS_PER_FRAME;
    frame->logical_frame_bits = (uint32_t)subchannel->bitrate_kbps * 24u;
    frame->coded_bits_per_cif = subchannel->softbits_per_cif;
    frame->coded_bits_per_frame = subchannel->softbits_per_cif * DAB_MSC_CIFS_PER_FRAME;

    for (cif_index = 0u; cif_index < DAB_MSC_CIFS_PER_FRAME; ++cif_index) {
        uint32_t offset = (cif_index * DAB_MSC_SOFTBITS_PER_CIF) + ((uint32_t)subchannel->start_cu * DAB_MSC_SOFTBITS_PER_CU);

        if (offset + subchannel->softbits_per_cif > msc_softbit_count) {
            return -1;
        }
        frame->cif_softbit_offset[cif_index] = offset;
        frame->cif_softbit_count[cif_index] = subchannel->softbits_per_cif;
    }

    return 0;
}

static void dab_msc_free_decoder_workspace(dab_msc_decoder_workspace_t *workspace) {
    if (workspace == 0) {
        return;
    }

    delete_viterbi47_port(workspace->viterbi);
    free(workspace->metrics);
    free(workspace->unpunctured);
    free(workspace->deinterleaved);
    free(workspace);
}

static dab_msc_decoder_workspace_t *dab_msc_create_decoder_workspace(
    const dab_msc_decoder_config_t *config) {
    dab_msc_decoder_workspace_t *workspace;
    uint32_t symbol_count;

    if (config == 0 || config->valid == 0u || config->conv_len == 0u || config->framebits == 0u) {
        return 0;
    }

    symbol_count = (config->framebits + 6u) * 4u;
    workspace = (dab_msc_decoder_workspace_t *)calloc(1u, sizeof(*workspace));
    if (workspace == 0) {
        return 0;
    }

    workspace->deinterleaved = (int8_t *)malloc(config->conv_len);
    workspace->unpunctured = (int16_t *)malloc(sizeof(*workspace->unpunctured) * symbol_count);
    workspace->metrics = (unsigned char *)malloc(symbol_count);
    workspace->viterbi = create_viterbi47_port((int)config->framebits);
    if (workspace->deinterleaved == 0 || workspace->unpunctured == 0 ||
        workspace->metrics == 0 || workspace->viterbi == 0) {
        dab_msc_free_decoder_workspace(workspace);
        return 0;
    }

    workspace->conv_len = config->conv_len;
    workspace->unpunctured_count = symbol_count;
    workspace->framebits = config->framebits;
    return workspace;
}

static void dab_msc_free_runtimes(dab_msc_subchannel_runtime_t *runtimes) {
    uint32_t i;

    if (runtimes == 0) {
        return;
    }
    for (i = 0u; i < DAB_FIC_MAX_SUBCHANNELS; ++i) {
        free(runtimes[i].deinterleaver);
        dab_msc_free_decoder_workspace(runtimes[i].workspace);
        dab_msc_free_dabplus_stream(runtimes[i].dabplus);
        runtimes[i].deinterleaver = 0;
        runtimes[i].workspace = 0;
        runtimes[i].dabplus = 0;
    }
}

static int dab_msc_depuncture_cif(const int8_t *punctured,
                                  uint32_t punctured_count,
                                  const dab_msc_decoder_config_t *config,
                                  int16_t *unpunctured,
                                  uint32_t unpunctured_count) {
    uint32_t in_pos = 0u;
    uint32_t out_pos = 0u;
    uint32_t segment_index;
    const uint32_t expected_unpunctured = (config->framebits + 6u) * 4u;

    if (punctured == 0 || config == 0 || unpunctured == 0 || config->valid == 0u ||
        punctured_count != config->conv_len || unpunctured_count < expected_unpunctured) {
        return -1;
    }

    for (segment_index = 0u; segment_index < config->segment_count; ++segment_index) {
        const dab_msc_puncture_segment_t *segment = &config->segments[segment_index];
        const uint32_t pattern = dab_msc_puncturing_code(segment->code_index);
        uint32_t remaining = segment->input_bits;

        if (pattern == 0u) {
            return -1;
        }

        while (remaining > 0u) {
            const uint32_t chunk_bits = remaining >= 32u ? 32u : remaining;
            uint32_t i;

            for (i = 0u; i < chunk_bits; ++i) {
                if (((pattern >> i) & 1u) != 0u) {
                    if (in_pos >= punctured_count) {
                        return -1;
                    }
                    unpunctured[out_pos + i] = punctured[in_pos++];
                } else {
                    unpunctured[out_pos + i] = 0;
                }
            }
            out_pos += chunk_bits;
            remaining -= chunk_bits;
        }
    }

    if (out_pos != expected_unpunctured) {
        return -1;
    }
    return in_pos + config->padding_bits == punctured_count ? 0 : -1;
}

static int dab_msc_viterbi_decode_preallocated(const int16_t *symbols,
                                               uint32_t framebits,
                                               uint8_t *bytes_out,
                                               uint32_t output_count,
                                               unsigned char *metrics,
                                               void *vp) {
    const uint32_t trellis_bits = framebits + 6u;
    const uint32_t symbol_count = trellis_bits * 4u;
    uint32_t i;

    if (symbols == 0 || bytes_out == 0 || metrics == 0 || vp == 0 ||
        output_count < (framebits / 8u)) {
        return -1;
    }

    for (i = 0u; i < symbol_count; ++i) {
        int value = (int)symbols[i] + 127;
        if (value < 0) {
            value = 0;
        } else if (value > 255) {
            value = 255;
        }
        metrics[i] = (unsigned char)value;
    }

    if (init_viterbi47_port(vp, 0) != 0 ||
        update_viterbi47_blk_port(vp, metrics, (int)trellis_bits) != 0) {
        return -1;
    }

    memset(bytes_out, 0, output_count);
    if (chainback_viterbi47_port(vp, bytes_out, framebits, 0) != 0) {
        return -1;
    }

    return 0;
}

static int dab_msc_viterbi_decode(const int16_t *symbols,
                                  uint32_t framebits,
                                  uint8_t *bytes_out,
                                  uint32_t output_count) {
    const uint32_t symbol_count = (framebits + 6u) * 4u;
    unsigned char *metrics;
    void *vp;
    int rc;

    if (symbols == 0 || bytes_out == 0 || output_count < (framebits / 8u)) {
        return -1;
    }

    metrics = (unsigned char *)malloc(symbol_count);
    if (metrics == 0) {
        return -1;
    }
    vp = create_viterbi47_port((int)framebits);
    if (vp == 0) {
        free(metrics);
        return -1;
    }

    rc = dab_msc_viterbi_decode_preallocated(symbols,
                                              framebits,
                                              bytes_out,
                                              output_count,
                                              metrics,
                                              vp);
    delete_viterbi47_port(vp);
    free(metrics);
    return rc;
}

static void dab_msc_descramble_bytes(uint8_t *data, uint32_t byte_count) {
    uint32_t i;

    for (i = 0u; i < byte_count; ++i) {
        data[i] = (uint8_t)(data[i] ^ dab_msc_prbs81(i == 0u));
    }
}

int dab_msc_prepare_decoder_config(dab_msc_decoder_config_t *config, const dab_msc_subchannel_t *subchannel) {
    if (config == 0 || subchannel == 0 || subchannel->valid == 0u) {
        return -1;
    }

    *config = (dab_msc_decoder_config_t){0};
    config->valid = 1u;
    config->subchid = subchannel->subchid;
    config->profile = subchannel->profile;
    config->protection_level = subchannel->protection_level;
    config->bitrate_kbps = subchannel->bitrate_kbps;
    config->size_cu = subchannel->size_cu;
    config->framebits = (uint32_t)subchannel->bitrate_kbps * 24u;

    if (subchannel->profile == DAB_MSC_PROFILE_UEP) {
        const dab_msc_uep_entry_t *entry;
        uint32_t i;

        if (subchannel->protection_index >= 64u) {
            return -1;
        }
        entry = &DAB_MSC_UEP_TABLE[subchannel->protection_index];
        if (entry->valid == 0u) {
            return -1;
        }
        for (i = 0u; i < 4u; ++i) {
            if (entry->pi[i] < 0 || entry->l[i] == 0u) {
                continue;
            }
            if (dab_msc_append_segment(config, (uint32_t)entry->l[i] * 128u, (uint8_t)entry->pi[i]) != 0) {
                return -1;
            }
        }
        if (dab_msc_append_segment(config, 24u, 25u) != 0) {
            return -1;
        }
        config->padding_bits = entry->padding;
        config->conv_len += entry->padding;
        if (config->framebits != entry->ic) {
            return -1;
        }
    } else if (subchannel->profile == DAB_MSC_PROFILE_EEP_A) {
        uint32_t n;

        switch (subchannel->protection_level) {
            case 1u:
                n = subchannel->size_cu / 12u;
                if (dab_msc_append_segment(config, ((6u * n) - 3u) * 128u, 24u) != 0 ||
                    dab_msc_append_segment(config, 3u * 128u, 23u) != 0 ||
                    dab_msc_append_segment(config, 24u, 25u) != 0) {
                    return -1;
                }
                break;
            case 2u:
                n = subchannel->size_cu / 8u;
                if (n == 1u) {
                    if (dab_msc_append_segment(config, 5u * 128u, 13u) != 0 ||
                        dab_msc_append_segment(config, 1u * 128u, 12u) != 0 ||
                        dab_msc_append_segment(config, 24u, 25u) != 0) {
                        return -1;
                    }
                } else {
                    if (dab_msc_append_segment(config, ((2u * n) - 3u) * 128u, 14u) != 0 ||
                        dab_msc_append_segment(config, ((4u * n) + 3u) * 128u, 13u) != 0 ||
                        dab_msc_append_segment(config, 24u, 25u) != 0) {
                        return -1;
                    }
                }
                break;
            case 3u:
                n = subchannel->size_cu / 6u;
                if (dab_msc_append_segment(config, ((6u * n) - 3u) * 128u, 8u) != 0 ||
                    dab_msc_append_segment(config, 3u * 128u, 7u) != 0 ||
                    dab_msc_append_segment(config, 24u, 25u) != 0) {
                    return -1;
                }
                break;
            case 4u:
                n = subchannel->size_cu / 4u;
                if (dab_msc_append_segment(config, ((4u * n) - 3u) * 128u, 3u) != 0 ||
                    dab_msc_append_segment(config, ((2u * n) + 3u) * 128u, 2u) != 0 ||
                    dab_msc_append_segment(config, 24u, 25u) != 0) {
                    return -1;
                }
                break;
            default:
                return -1;
        }
    } else if (subchannel->profile == DAB_MSC_PROFILE_EEP_B) {
        uint32_t n;

        switch (subchannel->protection_level) {
            case 1u:
                n = subchannel->size_cu / 27u;
                if (dab_msc_append_segment(config, ((24u * n) - 3u) * 128u, 10u) != 0 ||
                    dab_msc_append_segment(config, 3u * 128u, 9u) != 0 ||
                    dab_msc_append_segment(config, 24u, 25u) != 0) {
                    return -1;
                }
                break;
            case 2u:
                n = subchannel->size_cu / 21u;
                if (dab_msc_append_segment(config, ((24u * n) - 3u) * 128u, 6u) != 0 ||
                    dab_msc_append_segment(config, 3u * 128u, 5u) != 0 ||
                    dab_msc_append_segment(config, 24u, 25u) != 0) {
                    return -1;
                }
                break;
            case 3u:
                n = subchannel->size_cu / 18u;
                if (dab_msc_append_segment(config, ((24u * n) - 3u) * 128u, 4u) != 0 ||
                    dab_msc_append_segment(config, 3u * 128u, 3u) != 0 ||
                    dab_msc_append_segment(config, 24u, 25u) != 0) {
                    return -1;
                }
                break;
            case 4u:
                n = subchannel->size_cu / 15u;
                if (dab_msc_append_segment(config, ((24u * n) - 3u) * 128u, 2u) != 0 ||
                    dab_msc_append_segment(config, 3u * 128u, 1u) != 0 ||
                    dab_msc_append_segment(config, 24u, 25u) != 0) {
                    return -1;
                }
                break;
            default:
                return -1;
        }
    } else {
        return -1;
    }

    return config->conv_len == ((uint32_t)subchannel->size_cu * DAB_MSC_SOFTBITS_PER_CU) ? 0 : -1;
}

int dab_msc_sync_runtimes(dab_msc_subchannel_runtime_t *runtimes, const dab_msc_layout_t *layout) {
    uint32_t i;

    if (runtimes == 0 || layout == 0) {
        return -1;
    }

    for (i = 0u; i < DAB_FIC_MAX_SUBCHANNELS; ++i) {
        const dab_msc_subchannel_t *subchannel = &layout->subchannels[i];
        dab_msc_subchannel_runtime_t *runtime = &runtimes[i];

        if (subchannel->valid == 0u) {
            free(runtime->deinterleaver);
            dab_msc_free_decoder_workspace(runtime->workspace);
            dab_msc_free_dabplus_stream(runtime->dabplus);
            *runtime = (dab_msc_subchannel_runtime_t){0};
            continue;
        }

        if (runtime->valid == 0u || runtime->deinterleaver == 0 || runtime->workspace == 0 ||
            runtime->subchid != subchannel->subchid ||
            runtime->config.profile != subchannel->profile ||
            runtime->config.protection_level != subchannel->protection_level ||
            runtime->config.bitrate_kbps != subchannel->bitrate_kbps ||
            runtime->config.size_cu != subchannel->size_cu) {
            dab_msc_decoder_config_t config;
            dab_msc_deinterleaver_t *deinterleaver = runtime->deinterleaver;
            dab_msc_decoder_workspace_t *workspace;
            dab_msc_dabplus_stream_runtime_t *dabplus = runtime->dabplus;
            uint32_t reset_count = runtime->reset_count;
            uint8_t reason = 0u;
            int allocated_deinterleaver = 0;

            if (runtime->valid == 0u) {
                reason |= 0x01u;
            } else {
                if (runtime->subchid != subchannel->subchid) {
                    reason |= 0x02u;
                }
                if (runtime->config.profile != subchannel->profile) {
                    reason |= 0x04u;
                }
                if (runtime->config.protection_level != subchannel->protection_level) {
                    reason |= 0x08u;
                }
                if (runtime->config.bitrate_kbps != subchannel->bitrate_kbps) {
                    reason |= 0x10u;
                }
                if (runtime->config.size_cu != subchannel->size_cu) {
                    reason |= 0x20u;
                }
                reset_count++;
            }

            if (dab_msc_prepare_decoder_config(&config, subchannel) != 0) {
                free(runtime->deinterleaver);
                dab_msc_free_decoder_workspace(runtime->workspace);
                dab_msc_free_dabplus_stream(runtime->dabplus);
                *runtime = (dab_msc_subchannel_runtime_t){0};
                continue;
            }
            if (deinterleaver == 0) {
                deinterleaver = (dab_msc_deinterleaver_t *)calloc(1u, sizeof(*deinterleaver));
                if (deinterleaver == 0) {
                    return -1;
                }
                allocated_deinterleaver = 1;
            }
            workspace = dab_msc_create_decoder_workspace(&config);
            if (workspace == 0) {
                if (allocated_deinterleaver != 0) {
                    free(deinterleaver);
                }
                return -1;
            }
            dab_msc_free_decoder_workspace(runtime->workspace);
            if (dabplus != 0) {
                dab_msc_free_dabplus_stream(dabplus);
                dabplus = 0;
            }
            *runtime = (dab_msc_subchannel_runtime_t){0};
            runtime->valid = 1u;
            runtime->subchid = subchannel->subchid;
            runtime->config = config;
            runtime->reset_count = reset_count;
            runtime->last_reconfig_reason = reason;
            runtime->deinterleaver = deinterleaver;
            runtime->workspace = workspace;
            dab_msc_deinterleaver_reset(runtime->deinterleaver);
        }
    }

    return 0;
}

void dab_msc_deinterleaver_reset(dab_msc_deinterleaver_t *state) {
    if (state == 0) {
        return;
    }
    memset(state, 0, sizeof(*state));
}

int dab_msc_convolutional_deinterleave(dab_msc_deinterleaver_t *state,
                                       const int8_t *input,
                                       uint32_t input_count,
                                       int8_t *output,
                                       uint32_t output_count) {
    uint32_t i;

    if (state == 0 || input == 0 || output == 0 || input_count == 0u || input_count != output_count ||
        input_count > DAB_MSC_MAX_CONV_BITS) {
        return -1;
    }

    state->conv_len = input_count;
    for (i = 0u; i < input_count; ++i) {
        const uint32_t base = i * DAB_MSC_CONV_DEINTERLEAVER_DEPTH;
        const uint8_t delay = DAB_MSC_DEINTERLEAVER_DELAYS[i & 0x0Fu];
        uint32_t j;

        for (j = delay; j > 0u; --j) {
            state->regs[base + j] = state->regs[base + j - 1u];
        }
        state->regs[base] = input[i];
        output[i] = state->regs[base + delay];
    }

    return 0;
}

int dab_msc_deinterleave_subchannel_frame(const int8_t *msc_softbits,
                                          uint32_t msc_softbit_count,
                                          const dab_msc_subchannel_frame_t *frame,
                                          const dab_msc_decoder_config_t *config,
                                          dab_msc_deinterleaver_t *state,
                                          int8_t *output,
                                          uint32_t output_count) {
    uint32_t cif_index;

    if (msc_softbits == 0 || frame == 0 || config == 0 || state == 0 || output == 0 ||
        msc_softbit_count != DAB_TFRAME_MSC_SOFTBITS || frame->valid == 0u || config->valid == 0u ||
        config->conv_len != frame->coded_bits_per_cif ||
        output_count < (frame->coded_bits_per_cif * DAB_MSC_CIFS_PER_FRAME)) {
        return -1;
    }

    for (cif_index = 0u; cif_index < DAB_MSC_CIFS_PER_FRAME; ++cif_index) {
        const uint32_t offset = frame->cif_softbit_offset[cif_index];
        const uint32_t count = frame->cif_softbit_count[cif_index];

        if (offset + count > msc_softbit_count) {
            return -1;
        }
        if (dab_msc_convolutional_deinterleave(state,
                                               msc_softbits + offset,
                                               count,
                                               output + (cif_index * config->conv_len),
                                               config->conv_len) != 0) {
            return -1;
        }
    }

    return 0;
}

int dab_msc_decode_deinterleaved_cif(const int8_t *deinterleaved_softbits,
                                     uint32_t deinterleaved_count,
                                     const dab_msc_decoder_config_t *config,
                                     uint8_t *output_bytes,
                                     uint32_t output_count) {
    return dab_msc_decode_deinterleaved_cif_debug(deinterleaved_softbits,
                                                  deinterleaved_count,
                                                  config,
                                                  0,
                                                  0u,
                                                  0,
                                                  0u,
                                                  output_bytes,
                                                  output_count,
                                                  0);
}

int dab_msc_decode_deinterleaved_cif_debug(const int8_t *deinterleaved_softbits,
                                           uint32_t deinterleaved_count,
                                           const dab_msc_decoder_config_t *config,
                                           int16_t *unpunctured,
                                           uint32_t unpunctured_count,
                                           uint8_t *pre_descramble_bytes,
                                           uint32_t pre_descramble_count,
                                           uint8_t *post_descramble_bytes,
                                           uint32_t post_descramble_count,
                                           uint32_t *byte_count_out) {
    const uint32_t expected_unpunctured_count = (config->framebits + 6u) * 4u;
    const uint32_t byte_count = config->framebits / 8u;
    int16_t *unpunctured_storage = unpunctured;
    uint8_t *pre_descramble_storage = pre_descramble_bytes;
    int rc;

    if (deinterleaved_softbits == 0 || config == 0 || config->valid == 0u ||
        deinterleaved_count != config->conv_len) {
        return -1;
    }
    if (unpunctured_storage != 0 && unpunctured_count < expected_unpunctured_count) {
        return -1;
    }
    if (pre_descramble_storage != 0 && pre_descramble_count < byte_count) {
        return -1;
    }
    if (post_descramble_bytes != 0 && post_descramble_count < byte_count) {
        return -1;
    }

    if (unpunctured_storage == 0) {
        unpunctured_storage = (int16_t *)malloc(sizeof(*unpunctured_storage) * expected_unpunctured_count);
        if (unpunctured_storage == 0) {
            return -1;
        }
    }
    if (pre_descramble_storage == 0) {
        pre_descramble_storage = (uint8_t *)malloc(byte_count);
        if (pre_descramble_storage == 0) {
            if (unpunctured_storage != unpunctured) {
                free(unpunctured_storage);
            }
            return -1;
        }
    }

    rc = dab_msc_depuncture_cif(deinterleaved_softbits,
                                deinterleaved_count,
                                config,
                                unpunctured_storage,
                                expected_unpunctured_count);
    if (rc == 0) {
        rc = dab_msc_viterbi_decode(unpunctured_storage, config->framebits, pre_descramble_storage, byte_count);
    }
    if (rc == 0 && post_descramble_bytes != 0) {
        memcpy(post_descramble_bytes, pre_descramble_storage, byte_count);
        dab_msc_descramble_bytes(post_descramble_bytes, byte_count);
    } else if (rc == 0) {
        dab_msc_descramble_bytes(pre_descramble_storage, byte_count);
    }
    if (rc == 0 && byte_count_out != 0) {
        *byte_count_out = byte_count;
    }

    if (pre_descramble_storage != pre_descramble_bytes) {
        free(pre_descramble_storage);
    }
    if (unpunctured_storage != unpunctured) {
        free(unpunctured_storage);
    }

    return rc == 0 ? 0 : -1;
}

int dab_msc_decode_subchannel_cif(const int8_t *msc_softbits,
                                  uint32_t msc_softbit_count,
                                  const dab_msc_subchannel_frame_t *frame,
                                  const dab_msc_decoder_config_t *config,
                                  dab_msc_deinterleaver_t *state,
                                  uint32_t cif_index,
                                  uint8_t *output_bytes,
                                  uint32_t output_count) {
    int8_t *deinterleaved;
    int rc;

    if (msc_softbits == 0 || frame == 0 || config == 0 || state == 0 || output_bytes == 0 ||
        msc_softbit_count != DAB_TFRAME_MSC_SOFTBITS || cif_index >= DAB_MSC_CIFS_PER_FRAME ||
        frame->valid == 0u || config->valid == 0u || frame->cif_softbit_count[cif_index] != config->conv_len) {
        return -1;
    }

    deinterleaved = (int8_t *)malloc(config->conv_len);
    if (deinterleaved == 0) {
        return -1;
    }
    rc = dab_msc_convolutional_deinterleave(state,
                                            msc_softbits + frame->cif_softbit_offset[cif_index],
                                            frame->cif_softbit_count[cif_index],
                                            deinterleaved,
                                            config->conv_len);
    if (rc == 0) {
        rc = dab_msc_decode_deinterleaved_cif(deinterleaved,
                                              config->conv_len,
                                              config,
                                              output_bytes,
                                              output_count);
    }
    free(deinterleaved);
    return rc;
}

static int dab_msc_decode_subchannel_cif_runtime(const int8_t *msc_softbits,
                                                 uint32_t msc_softbit_count,
                                                 const dab_msc_subchannel_frame_t *frame,
                                                 dab_msc_subchannel_runtime_t *runtime,
                                                 uint32_t cif_index,
                                                 uint8_t *output_bytes,
                                                 uint32_t output_count) {
    const dab_msc_decoder_config_t *config;
    dab_msc_decoder_workspace_t *workspace;
    int rc;

    if (msc_softbits == 0 || frame == 0 || runtime == 0 || output_bytes == 0 ||
        runtime->valid == 0u || runtime->deinterleaver == 0 || runtime->workspace == 0 ||
        msc_softbit_count != DAB_TFRAME_MSC_SOFTBITS || cif_index >= DAB_MSC_CIFS_PER_FRAME ||
        frame->valid == 0u) {
        return -1;
    }

    config = &runtime->config;
    workspace = runtime->workspace;
    if (config->valid == 0u || frame->cif_softbit_count[cif_index] != config->conv_len ||
        workspace->conv_len != config->conv_len || workspace->framebits != config->framebits ||
        workspace->unpunctured_count < ((config->framebits + 6u) * 4u)) {
        return -1;
    }

    rc = dab_msc_convolutional_deinterleave(runtime->deinterleaver,
                                             msc_softbits + frame->cif_softbit_offset[cif_index],
                                             frame->cif_softbit_count[cif_index],
                                             workspace->deinterleaved,
                                             config->conv_len);
    if (rc == 0) {
        rc = dab_msc_depuncture_cif(workspace->deinterleaved,
                                    config->conv_len,
                                    config,
                                    workspace->unpunctured,
                                    workspace->unpunctured_count);
    }
    if (rc == 0) {
        rc = dab_msc_viterbi_decode_preallocated(workspace->unpunctured,
                                                  config->framebits,
                                                  output_bytes,
                                                  output_count,
                                                  workspace->metrics,
                                                  workspace->viterbi);
    }
    if (rc == 0) {
        dab_msc_descramble_bytes(output_bytes, config->framebits / 8u);
    }
    return rc == 0 ? 0 : -1;
}

int dab_msc_init(dab_msc_state_t *state) {
    if (state == 0) {
        return -1;
    }
    *state = (dab_msc_state_t){0};
    return 0;
}

void dab_msc_reset(dab_msc_state_t *state) {
    uint8_t service_filter_enabled;
    uint32_t service_filter_sid;

    if (state == 0) {
        return;
    }
    service_filter_enabled = state->service_filter_enabled;
    service_filter_sid = state->service_filter_sid;
    dab_msc_free_runtimes(state->runtimes);
    *state = (dab_msc_state_t){0};
    state->service_filter_enabled = service_filter_enabled;
    state->service_filter_sid = service_filter_sid;
}

void dab_msc_set_service_filter(dab_msc_state_t *state, uint32_t sid) {
    if (state == 0) {
        return;
    }
    state->service_filter_enabled = 1u;
    state->service_filter_sid = sid;
}

void dab_msc_clear_service_filter(dab_msc_state_t *state) {
    if (state == 0) {
        return;
    }
    state->service_filter_enabled = 0u;
    state->service_filter_sid = 0u;
}

int dab_msc_build_layout(dab_msc_layout_t *layout, const dab_fic_database_t *db) {
    uint32_t i;

    if (layout == 0 || db == 0) {
        return -1;
    }

    *layout = (dab_msc_layout_t){0};

    for (i = 0u; i < DAB_FIC_MAX_SUBCHANNELS; ++i) {
        dab_msc_subchannel_t subchannel;

        if (db->subchannels[i].valid == 0u) {
            continue;
        }
        if (dab_msc_resolve_subchannel(&db->subchannels[i], &subchannel) != 0) {
            continue;
        }
        layout->subchannels[subchannel.subchid] = subchannel;
        layout->subchannel_count++;
    }

    for (i = 0u; i < db->service_count; ++i) {
        const dab_fic_service_t *service = &db->services[i];
        uint32_t component_index;

        for (component_index = 0u; component_index < service->component_count; ++component_index) {
            const dab_fic_service_component_t *component = &service->components[component_index];
            dab_msc_component_mapping_t *mapping;

            if (component->used == 0u || component->has_subchid == 0u || component->subchid >= DAB_FIC_MAX_SUBCHANNELS) {
                continue;
            }
            if (layout->subchannels[component->subchid].valid == 0u) {
                continue;
            }
            if (layout->component_count >= DAB_MSC_MAX_COMPONENT_MAPPINGS) {
                return -1;
            }

            mapping = &layout->components[layout->component_count++];
            *mapping = (dab_msc_component_mapping_t){0};
            mapping->used = 1u;
            mapping->is_programme = service->is_programme;
            mapping->service_index = (uint8_t)i;
            mapping->component_index = (uint8_t)component_index;
            mapping->subchid = component->subchid;
            mapping->primary = component->primary;
            mapping->tmid = component->tmid;
            mapping->sc_type = component->sc_type;
            mapping->packet_addr = component->packet_addr;
            mapping->has_packet_addr = component->has_packet_addr;
            mapping->dg_flag = component->dg_flag;
            mapping->caorg = component->caorg;
            mapping->has_caorg = component->has_caorg;
            mapping->sid = service->sid;
        }
    }

    return 0;
}

int dab_msc_build_frame_view(dab_msc_frame_view_t *frame_view,
                             const dab_msc_layout_t *layout,
                             uint32_t msc_softbit_count) {
    uint32_t i;

    if (frame_view == 0 || layout == 0 || msc_softbit_count != DAB_TFRAME_MSC_SOFTBITS) {
        return -1;
    }

    *frame_view = (dab_msc_frame_view_t){0};

    for (i = 0u; i < DAB_FIC_MAX_SUBCHANNELS; ++i) {
        const dab_msc_subchannel_t *subchannel = &layout->subchannels[i];
        dab_msc_subchannel_frame_t *frame;

        if (subchannel->valid == 0u) {
            continue;
        }
        frame = &frame_view->subchannels[subchannel->subchid];
        if (dab_msc_fill_subchannel_frame(subchannel, msc_softbit_count, frame) != 0) {
            return -1;
        }
        frame_view->subchannel_count++;
    }

    for (i = 0u; i < layout->component_count; ++i) {
        const dab_msc_component_mapping_t *mapping = &layout->components[i];
        const dab_msc_subchannel_frame_t *subchannel_frame;
        dab_msc_component_frame_t *component_frame;

        if (mapping->used == 0u || mapping->subchid >= DAB_FIC_MAX_SUBCHANNELS) {
            continue;
        }
        subchannel_frame = &frame_view->subchannels[mapping->subchid];
        if (subchannel_frame->valid == 0u) {
            continue;
        }
        component_frame = &frame_view->components[frame_view->component_count++];
        *component_frame = (dab_msc_component_frame_t){0};
        component_frame->used = 1u;
        component_frame->service_index = mapping->service_index;
        component_frame->component_index = mapping->component_index;
        component_frame->subchid = mapping->subchid;
        component_frame->primary = mapping->primary;
        component_frame->tmid = mapping->tmid;
        component_frame->sc_type = mapping->sc_type;
        component_frame->packet_addr = mapping->packet_addr;
        component_frame->has_packet_addr = mapping->has_packet_addr;
        component_frame->dg_flag = mapping->dg_flag;
        component_frame->caorg = mapping->caorg;
        component_frame->has_caorg = mapping->has_caorg;
        component_frame->sid = mapping->sid;
        component_frame->logical_frame_bits = subchannel_frame->logical_frame_bits;
        component_frame->coded_bits_per_cif = subchannel_frame->coded_bits_per_cif;
        for (uint32_t cif_index = 0u; cif_index < DAB_MSC_CIFS_PER_FRAME; ++cif_index) {
            component_frame->cif_softbit_offset[cif_index] = subchannel_frame->cif_softbit_offset[cif_index];
            component_frame->cif_softbit_count[cif_index] = subchannel_frame->cif_softbit_count[cif_index];
        }
    }

    return 0;
}

const dab_msc_subchannel_t *dab_msc_find_subchannel(const dab_msc_layout_t *layout, uint8_t subchid) {
    if (layout == 0 || subchid >= DAB_FIC_MAX_SUBCHANNELS || layout->subchannels[subchid].valid == 0u) {
        return 0;
    }
    return &layout->subchannels[subchid];
}

const dab_msc_subchannel_frame_t *dab_msc_find_subchannel_frame(const dab_msc_frame_view_t *frame_view, uint8_t subchid) {
    if (frame_view == 0 || subchid >= DAB_FIC_MAX_SUBCHANNELS || frame_view->subchannels[subchid].valid == 0u) {
        return 0;
    }
    return &frame_view->subchannels[subchid];
}

dab_msc_component_kind_t dab_msc_component_kind(uint8_t tmid) {
    switch (tmid) {
        case 0u:
            return DAB_MSC_COMPONENT_STREAM_AUDIO;
        case 1u:
            return DAB_MSC_COMPONENT_STREAM_DATA;
        case 3u:
            return DAB_MSC_COMPONENT_PACKET_MODE;
        default:
            return DAB_MSC_COMPONENT_UNKNOWN;
    }
}

const dab_msc_decoded_component_t *dab_msc_find_decoded_component(const dab_msc_state_t *state,
                                                                  uint32_t sid,
                                                                  uint8_t component_index) {
    uint32_t i;

    if (state == 0) {
        return 0;
    }

    for (i = 0u; i < state->decoded_component_count; ++i) {
        const dab_msc_decoded_component_t *component = &state->decoded_components[i];

        if (component->used != 0u &&
            component->sid == sid &&
            component->component_index == component_index) {
            return component;
        }
    }

    return 0;
}

const dab_msc_decoded_component_t *dab_msc_find_primary_decoded_component(const dab_msc_state_t *state,
                                                                          uint32_t sid) {
    uint32_t i;

    if (state == 0) {
        return 0;
    }

    for (i = 0u; i < state->decoded_component_count; ++i) {
        const dab_msc_decoded_component_t *component = &state->decoded_components[i];

        if (component->used != 0u &&
            component->sid == sid &&
            component->primary != 0u) {
            return component;
        }
    }

    return dab_msc_find_decoded_component(state, sid, 0u);
}

int dab_msc_get_component_payload(const dab_msc_state_t *state,
                                  const dab_msc_decoded_component_t *component,
                                  dab_msc_component_payload_t *payload) {
    const dab_msc_decoded_subchannel_t *decoded_subchannel;
    uint32_t cif_index;

    if (state == 0 || component == 0 || payload == 0 ||
        component->used == 0u || component->subchid >= DAB_FIC_MAX_SUBCHANNELS) {
        return -1;
    }

    decoded_subchannel = &state->decoded_subchannels[component->subchid];
    if (decoded_subchannel->valid == 0u) {
        return -1;
    }

    *payload = (dab_msc_component_payload_t){0};
    payload->valid = 1u;
    payload->service_index = component->service_index;
    payload->component_index = component->component_index;
    payload->subchid = component->subchid;
    payload->primary = component->primary;
    payload->tmid = component->tmid;
    payload->sc_type = component->sc_type;
    payload->sid = component->sid;
    payload->kind = dab_msc_component_kind(component->tmid);
    payload->packet_addr = component->packet_addr;
    payload->has_packet_addr = component->has_packet_addr;
    payload->dg_flag = component->dg_flag;
    payload->caorg = component->caorg;
    payload->has_caorg = component->has_caorg;
    for (cif_index = 0u; cif_index < DAB_MSC_CIFS_PER_FRAME; ++cif_index) {
        payload->cif_valid[cif_index] = decoded_subchannel->cif_valid[cif_index];
        payload->byte_count[cif_index] = decoded_subchannel->byte_count[cif_index];
        payload->data[cif_index] = decoded_subchannel->cif_valid[cif_index] != 0u
                                       ? decoded_subchannel->data[cif_index]
                                       : 0;
    }

    return 0;
}

int dab_msc_get_primary_component_payload(const dab_msc_state_t *state,
                                          uint32_t sid,
                                          dab_msc_component_payload_t *payload) {
    const dab_msc_decoded_component_t *component = dab_msc_find_primary_decoded_component(state, sid);

    if (component == 0) {
        return -1;
    }

    return dab_msc_get_component_payload(state, component, payload);
}

int dab_msc_ingest_component_payload(dab_msc_state_t *state,
                                     const dab_msc_decoded_component_t *component,
                                     const dab_msc_component_payload_t *payload) {
    dab_msc_subchannel_runtime_t *runtime;
    dab_msc_dabplus_stream_runtime_t *dabplus;
    uint32_t cif_index;

    if (state == 0 || component == 0 || payload == 0 || component->used == 0u || payload->valid == 0u ||
        component->subchid != payload->subchid || component->subchid >= DAB_FIC_MAX_SUBCHANNELS) {
        return -1;
    }
    if (dab_msc_component_kind(component->tmid) != DAB_MSC_COMPONENT_STREAM_AUDIO) {
        return 0;
    }

    runtime = &state->runtimes[component->subchid];
    if (runtime->valid == 0u) {
        return -1;
    }

    for (cif_index = 0u; cif_index < DAB_MSC_CIFS_PER_FRAME; ++cif_index) {
        if (payload->cif_valid[cif_index] == 0u || payload->byte_count[cif_index] == 0u || payload->data[cif_index] == 0) {
            continue;
        }

        dabplus = dab_msc_ensure_dabplus_stream(runtime, payload->byte_count[cif_index]);
        if (dabplus == 0) {
            return -1;
        }
        if (dab_msc_dabplus_feed_cif(dabplus,
                                     payload->data[cif_index],
                                     payload->byte_count[cif_index]) != 0) {
            dab_msc_reset_dabplus_stream(dabplus);
            return -1;
        }
    }

    return 0;
}

int dab_msc_get_dabplus_superframe(const dab_msc_state_t *state,
                                   const dab_msc_decoded_component_t *component,
                                   dab_msc_dabplus_superframe_t *superframe) {
    const dab_msc_subchannel_runtime_t *runtime;
    const dab_msc_dabplus_stream_runtime_t *dabplus;
    uint32_t i;

    if (state == 0 || component == 0 || superframe == 0 ||
        component->used == 0u || dab_msc_component_kind(component->tmid) != DAB_MSC_COMPONENT_STREAM_AUDIO ||
        component->subchid >= DAB_FIC_MAX_SUBCHANNELS) {
        return -1;
    }

    runtime = &state->runtimes[component->subchid];
    dabplus = runtime->dabplus;
    if (runtime->valid == 0u || dabplus == 0 || dabplus->superframe_valid == 0u || dabplus->synchronized == 0u) {
        return -1;
    }

    *superframe = (dab_msc_dabplus_superframe_t){0};
    superframe->valid = 1u;
    superframe->synchronized = dabplus->synchronized;
    superframe->generation = dabplus->generation;
    superframe->service_index = component->service_index;
    superframe->component_index = component->component_index;
    superframe->subchid = component->subchid;
    superframe->primary = component->primary;
    superframe->tmid = component->tmid;
    superframe->sc_type = component->sc_type;
    superframe->sid = component->sid;
    superframe->cif_bytes = dabplus->cif_bytes;
    superframe->superframe_bytes = dabplus->superframe_bytes;
    superframe->dac_rate = dabplus->dac_rate;
    superframe->sbr_flag = dabplus->sbr_flag;
    superframe->aac_channel_mode = dabplus->aac_channel_mode;
    superframe->ps_flag = dabplus->ps_flag;
    superframe->mpeg_surround_config = dabplus->mpeg_surround_config;
    superframe->au_count = dabplus->au_count;
    for (i = 0u; i < DAB_MSC_DABPLUS_MAX_AUS; ++i) {
        superframe->au_start[i] = dabplus->au_start[i];
        superframe->au_length[i] = dabplus->au_length[i];
    }
    superframe->superframe_data = dabplus->superframe;
    return 0;
}

int dab_msc_get_primary_dabplus_superframe(const dab_msc_state_t *state,
                                           uint32_t sid,
                                           dab_msc_dabplus_superframe_t *superframe) {
    const dab_msc_decoded_component_t *component = dab_msc_find_primary_decoded_component(state, sid);

    if (component == 0) {
        return -1;
    }

    return dab_msc_get_dabplus_superframe(state, component, superframe);
}

int dab_msc_get_dabplus_debug(const dab_msc_state_t *state,
                              const dab_msc_decoded_component_t *component,
                              dab_msc_dabplus_debug_t *debug) {
    const dab_msc_subchannel_runtime_t *runtime;
    const dab_msc_dabplus_stream_runtime_t *dabplus;

    if (state == 0 || component == 0 || debug == 0 ||
        component->used == 0u || dab_msc_component_kind(component->tmid) != DAB_MSC_COMPONENT_STREAM_AUDIO ||
        component->subchid >= DAB_FIC_MAX_SUBCHANNELS) {
        return -1;
    }

    *debug = (dab_msc_dabplus_debug_t){0};
    runtime = &state->runtimes[component->subchid];
    if (runtime->valid == 0u) {
        return -1;
    }

    debug->valid = 1u;
    debug->subchid = component->subchid;
    dabplus = runtime->dabplus;
    if (dabplus == 0) {
        return 0;
    }

    debug->cif_bytes = dabplus->cif_bytes;
    debug->columns_filled = dabplus->columns_filled;
    debug->synchronized = dabplus->synchronized;
    debug->superframe_valid = dabplus->superframe_valid;
    debug->generation = dabplus->generation;
    debug->decode_attempts = dabplus->decode_attempts;
    debug->decode_failures = dabplus->decode_failures;
    debug->rs_corrected_last_superframe = dabplus->rs_corrected_last_superframe;
    debug->rs_corrected_total = dabplus->rs_corrected_total;
    debug->last_decode_ok = dabplus->last_decode_ok;
    debug->runtime_reset_count = runtime->reset_count;
    debug->last_reconfig_reason = runtime->last_reconfig_reason;
    return 0;
}

int dab_msc_get_primary_dabplus_debug(const dab_msc_state_t *state,
                                      uint32_t sid,
                                      dab_msc_dabplus_debug_t *debug) {
    const dab_msc_decoded_component_t *component = dab_msc_find_primary_decoded_component(state, sid);

    if (component == 0) {
        return -1;
    }

    return dab_msc_get_dabplus_debug(state, component, debug);
}

int dab_msc_get_subchannel_softbits(const int8_t *msc_softbits,
                                    uint32_t msc_softbit_count,
                                    const dab_msc_subchannel_t *subchannel,
                                    uint32_t cif_index,
                                    const int8_t **slice,
                                    uint32_t *slice_count) {
    uint32_t offset;

    if (msc_softbits == 0 || subchannel == 0 || slice == 0 || slice_count == 0 ||
        subchannel->valid == 0u || cif_index >= DAB_MSC_CIFS_PER_FRAME ||
        msc_softbit_count != DAB_TFRAME_MSC_SOFTBITS) {
        return -1;
    }

    offset = (cif_index * DAB_MSC_SOFTBITS_PER_CIF) + ((uint32_t)subchannel->start_cu * DAB_MSC_SOFTBITS_PER_CU);
    if (offset + subchannel->softbits_per_cif > msc_softbit_count) {
        return -1;
    }

    *slice = msc_softbits + offset;
    *slice_count = subchannel->softbits_per_cif;
    return 0;
}

int dab_msc_process_frame(dab_msc_state_t *state,
                          uint64_t frame_start_sample,
                          const int8_t *msc_softbits,
                          uint32_t msc_softbit_count) {
    if (state == 0 || msc_softbits == 0 || msc_softbit_count != DAB_TFRAME_MSC_SOFTBITS) {
        return -1;
    }

    state->frames_seen++;
    state->last_frame_start_sample = frame_start_sample;
    state->first_softbit = msc_softbits[0];
    state->last_softbit = msc_softbits[msc_softbit_count - 1u];
    return 0;
}

int dab_msc_process_frame_with_fic(dab_msc_state_t *state,
                                   uint64_t frame_start_sample,
                                   const int8_t *msc_softbits,
                                   uint32_t msc_softbit_count,
                                   const dab_fic_database_t *db) {
    uint32_t i;
    dab_msc_layout_t filtered_layout;
    const dab_msc_layout_t *runtime_layout;
    const char *diag_subch_env = getenv("DAB_MSC_DIAG_SUBCHID");
    int diag_subch = -1;

    if (diag_subch_env != 0 && diag_subch_env[0] != '\0') {
        diag_subch = atoi(diag_subch_env);
    }

    if (db == 0) {
        return -1;
    }
    if (dab_msc_process_frame(state, frame_start_sample, msc_softbits, msc_softbit_count) != 0) {
        return -1;
    }
    if (dab_msc_build_layout(&state->layout, db) != 0) {
        return -1;
    }
    if (dab_msc_build_frame_view(&state->frame_view, &state->layout, msc_softbit_count) != 0) {
        return -1;
    }
    runtime_layout = &state->layout;
    if (state->service_filter_enabled != 0u) {
        uint8_t selected_subchid = 0xffu;

        filtered_layout = state->layout;
        for (i = 0u; i < filtered_layout.component_count; ++i) {
            const dab_msc_component_mapping_t *component = &filtered_layout.components[i];

            if (component->used != 0u && component->sid == state->service_filter_sid &&
                component->subchid < DAB_FIC_MAX_SUBCHANNELS &&
                dab_msc_component_kind(component->tmid) == DAB_MSC_COMPONENT_STREAM_AUDIO) {
                selected_subchid = component->subchid;
                if (component->primary != 0u) {
                    break;
                }
            }
        }

        filtered_layout.subchannel_count = 0u;
        for (i = 0u; i < DAB_FIC_MAX_SUBCHANNELS; ++i) {
            if (i != selected_subchid) {
                filtered_layout.subchannels[i].valid = 0u;
            } else if (filtered_layout.subchannels[i].valid != 0u) {
                filtered_layout.subchannel_count = 1u;
            }
        }
        runtime_layout = &filtered_layout;
    }
    if (dab_msc_sync_runtimes(state->runtimes, runtime_layout) != 0) {
        return -1;
    }

    for (i = 0u; i < DAB_FIC_MAX_SUBCHANNELS; ++i) {
        state->decoded_subchannels[i].valid = 0u;
        state->decoded_subchannels[i].subchid = 0u;
        memset(state->decoded_subchannels[i].cif_valid, 0, sizeof(state->decoded_subchannels[i].cif_valid));
        memset(state->decoded_subchannels[i].byte_count, 0, sizeof(state->decoded_subchannels[i].byte_count));
    }
    memset(state->decoded_components, 0, sizeof(state->decoded_components));
    state->decoded_component_count = 0u;

    for (i = 0u; i < DAB_FIC_MAX_SUBCHANNELS; ++i) {
        const dab_msc_subchannel_frame_t *frame = &state->frame_view.subchannels[i];
        dab_msc_subchannel_runtime_t *runtime = &state->runtimes[i];
        dab_msc_decoded_subchannel_t *decoded = &state->decoded_subchannels[i];
        uint32_t cif_index;

        if (frame->valid == 0u || runtime->valid == 0u) {
            continue;
        }

        decoded->valid = 1u;
        decoded->subchid = frame->subchid;
        for (cif_index = 0u; cif_index < DAB_MSC_CIFS_PER_FRAME; ++cif_index) {
            const uint32_t byte_count = runtime->config.framebits / 8u;
            uint8_t *decoded_bytes = decoded->data[cif_index];
            dab_msc_deinterleaver_t state_before_decode;
            int have_state_before_decode = 0;

            if (byte_count > DAB_MSC_MAX_LOGICAL_FRAME_BYTES_PER_CIF) {
                return -1;
            }
            if (diag_subch >= 0 && (int)frame->subchid == diag_subch) {
                state_before_decode = *runtime->deinterleaver;
                have_state_before_decode = 1;
            }
            memset(decoded_bytes, 0, byte_count);
            if (dab_msc_decode_subchannel_cif_runtime(msc_softbits,
                                                      msc_softbit_count,
                                                      frame,
                                                      runtime,
                                                      cif_index,
                                                      decoded_bytes,
                                                      byte_count) == 0) {
                if (have_state_before_decode != 0) {
                    uint8_t verify_bytes[DAB_MSC_MAX_LOGICAL_FRAME_BYTES_PER_CIF];

                    memset(verify_bytes, 0, sizeof(verify_bytes));
                    if (dab_msc_decode_subchannel_cif(msc_softbits,
                                                      msc_softbit_count,
                                                      frame,
                                                      &runtime->config,
                                                      &state_before_decode,
                                                      cif_index,
                                                      verify_bytes,
                                                      byte_count) == 0 &&
                        memcmp(decoded->data[cif_index], verify_bytes, byte_count) != 0) {
                        fprintf(stderr,
                                "msc-inline-store-mismatch frame_start=%llu subch=%u cif=%u stored=0x%02x verify=0x%02x\n",
                                (unsigned long long)frame_start_sample,
                                (unsigned)frame->subchid,
                                cif_index,
                                decoded->data[cif_index][0],
                                verify_bytes[0]);
                    }
                }
                decoded->cif_valid[cif_index] = 1u;
                decoded->byte_count[cif_index] = (uint16_t)byte_count;
            }
        }
    }

    for (i = 0u; i < state->frame_view.component_count; ++i) {
        const dab_msc_component_frame_t *component = &state->frame_view.components[i];
        dab_msc_decoded_component_t *decoded_component;

        if (component->used == 0u || component->subchid >= DAB_FIC_MAX_SUBCHANNELS ||
            state->decoded_component_count >= DAB_MSC_MAX_COMPONENT_MAPPINGS) {
            continue;
        }

        decoded_component = &state->decoded_components[state->decoded_component_count++];
        *decoded_component = (dab_msc_decoded_component_t){0};
        decoded_component->used = 1u;
        decoded_component->service_index = component->service_index;
        decoded_component->component_index = component->component_index;
        decoded_component->subchid = component->subchid;
        decoded_component->primary = component->primary;
        decoded_component->tmid = component->tmid;
        decoded_component->sc_type = component->sc_type;
        decoded_component->packet_addr = component->packet_addr;
        decoded_component->has_packet_addr = component->has_packet_addr;
        decoded_component->dg_flag = component->dg_flag;
        decoded_component->caorg = component->caorg;
        decoded_component->has_caorg = component->has_caorg;
        decoded_component->sid = component->sid;
    }

    {
        uint8_t fed_subchannels[DAB_FIC_MAX_SUBCHANNELS] = {0};

        for (i = 0u; i < state->decoded_component_count; ++i) {
            const dab_msc_decoded_component_t *component = &state->decoded_components[i];
            dab_msc_component_payload_t payload;

            if (component->used == 0u ||
                dab_msc_component_kind(component->tmid) != DAB_MSC_COMPONENT_STREAM_AUDIO ||
                component->subchid >= DAB_FIC_MAX_SUBCHANNELS ||
                fed_subchannels[component->subchid] != 0u) {
                continue;
            }
            if (dab_msc_get_component_payload(state, component, &payload) == 0 &&
                dab_msc_ingest_component_payload(state, component, &payload) != 0) {
                return -1;
            }

            fed_subchannels[component->subchid] = 1u;
        }
    }

    return 0;
}
