#include "dab/core/msc.h"

#include <stdlib.h>
#include <string.h>

#include "test_support.h"

static uint8_t test_msc_prbs_bit(int reset) {
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

static uint8_t test_msc_prbs81(uint32_t reset) {
    uint8_t value = 0u;
    uint32_t i;

    for (i = 0u; i < 8u; ++i) {
        value = (uint8_t)((value << 1u) | test_msc_prbs_bit(reset != 0u && i == 0u));
    }
    return value;
}

static uint8_t test_msc_mother_code(uint8_t b, uint8_t *s) {
    uint8_t x0;
    uint8_t x1;
    uint8_t x2;
    uint8_t x3;
    uint8_t r;

    x0 = (uint8_t)((((*s >> 5u) & 1u) ^ ((*s >> 4u) & 1u) ^ ((*s >> 2u) & 1u) ^ ((*s >> 1u) & 1u) ^ (b & 1u)) & 1u);
    x1 = (uint8_t)((((*s >> 5u) & 1u) ^ ((*s >> 2u) & 1u) ^ ((*s >> 1u) & 1u) ^ ((*s >> 0u) & 1u) ^ (b & 1u)) & 1u);
    x2 = (uint8_t)((((*s >> 5u) & 1u) ^ ((*s >> 3u) & 1u) ^ ((*s >> 0u) & 1u) ^ (b & 1u)) & 1u);
    x3 = x0;
    r = (uint8_t)((x0 & 1u) | ((x1 & 1u) << 1u) | ((x2 & 1u) << 2u) | ((x3 & 1u) << 3u));
    *s = (uint8_t)(((*s << 1u) & 0x3Fu) | (b & 1u));
    return r;
}

static uint32_t test_msc_cc_encoder(uint8_t b, uint8_t reset) {
    static uint8_t s = 0u;
    uint32_t r = 0u;
    uint32_t i;

    if (reset != 0u) {
        s = 0u;
    }
    for (i = 0u; i < 8u; ++i) {
        const uint8_t m = test_msc_mother_code((uint8_t)((b >> i) & 1u), &s);
        r |= ((uint32_t)m & 0x0Fu) << (4u * i);
    }
    return r;
}

static uint32_t test_msc_puncturing_code(uint8_t index) {
    static const uint32_t table[26] = {
        0u,
        0x11111113u, 0x11131113u, 0x11131313u, 0x13131313u, 0x13131333u,
        0x13331333u, 0x13333333u, 0x33333333u, 0x73333333u, 0x73337333u,
        0x73737333u, 0x73737373u, 0x37373777u, 0x37773777u, 0x37777777u,
        0x77777777u, 0xF7777777u, 0xF777F777u, 0xF7F7F777u, 0xF7F7F7F7u,
        0xFFF7F7F7u, 0xFFF7FFF7u, 0xFFFFFFF7u, 0xFFFFFFFFu, 0x00333333u
    };
    return table[index];
}

static void test_msc_build_deinterleaved_cif(const uint8_t *plain,
                                             const dab_msc_decoder_config_t *config,
                                             int8_t *softbits_out) {
    uint8_t scrambled[64] = {0};
    uint8_t byte_count = (uint8_t)(config->framebits / 8u);
    uint8_t mother_bits[DAB_MSC_MAX_CONV_BITS] = {0};
    uint32_t mother_pos = 0u;
    uint32_t out_pos = 0u;
    uint32_t i;

    for (i = 0u; i < byte_count; ++i) {
        scrambled[i] = (uint8_t)(plain[i] ^ test_msc_prbs81(i == 0u));
    }
    for (i = 0u; i < byte_count; ++i) {
        const uint32_t code = test_msc_cc_encoder(scrambled[i], i == 0u);
        uint32_t j;
        for (j = 0u; j < 32u; ++j) {
            mother_bits[mother_pos++] = (uint8_t)((code >> j) & 1u);
        }
    }
    {
        const uint32_t tail = test_msc_cc_encoder(0u, 0u);
        uint32_t j;
        for (j = 0u; j < 24u; ++j) {
            mother_bits[mother_pos++] = (uint8_t)((tail >> j) & 1u);
        }
    }

    mother_pos = 0u;
    for (i = 0u; i < config->segment_count; ++i) {
        const dab_msc_puncture_segment_t *segment = &config->segments[i];
        const uint32_t pattern = test_msc_puncturing_code(segment->code_index);
        uint32_t remaining = segment->input_bits;

        while (remaining > 0u) {
            const uint32_t chunk_bits = remaining >= 32u ? 32u : remaining;
            uint32_t j;
            for (j = 0u; j < chunk_bits; ++j) {
                if (((pattern >> j) & 1u) != 0u) {
                    softbits_out[out_pos++] = mother_bits[mother_pos + j] != 0u ? 127 : -127;
                }
            }
            mother_pos += chunk_bits;
            remaining -= chunk_bits;
        }
    }
    for (i = 0u; i < config->padding_bits; ++i) {
        softbits_out[out_pos++] = -127;
    }
}

static uint8_t test_msc_rs_log[256];
static uint8_t test_msc_rs_binary[256];
static uint8_t test_msc_rs_init_done = 0u;

static void test_msc_rs_init(void) {
    uint16_t r = 1u;
    uint32_t i;

    if (test_msc_rs_init_done != 0u) {
        return;
    }
    memset(test_msc_rs_log, 0, sizeof(test_msc_rs_log));
    memset(test_msc_rs_binary, 0, sizeof(test_msc_rs_binary));
    for (i = 0u; i < 255u; ++i) {
        test_msc_rs_log[r] = (uint8_t)i;
        test_msc_rs_binary[i] = (uint8_t)r;
        r <<= 1u;
        if (r >= 256u) {
            r &= 0xFFu;
            r ^= (1u << 4u) | (1u << 3u) | (1u << 2u) | 1u;
        }
    }
    test_msc_rs_init_done = 1u;
}

static uint8_t test_msc_rs_mul(uint8_t a, uint8_t b) {
    if (a == 0u || b == 0u) {
        return 0u;
    }
    test_msc_rs_init();
    return test_msc_rs_binary[(test_msc_rs_log[a] + test_msc_rs_log[b]) % 255u];
}

static void test_msc_rs_encode_row_245_10(const uint8_t payload[245], uint8_t parity[10]) {
    static const uint8_t poly[10] = {193u, 157u, 113u, 95u, 94u, 199u, 111u, 159u, 194u, 216u};
    uint8_t z[10] = {0};
    uint32_t i;

    for (i = 0u; i < 245u; ++i) {
        const uint8_t t = payload[i] ^ z[9];

        z[9] = test_msc_rs_mul(poly[9], t) ^ z[8];
        z[8] = test_msc_rs_mul(poly[8], t) ^ z[7];
        z[7] = test_msc_rs_mul(poly[7], t) ^ z[6];
        z[6] = test_msc_rs_mul(poly[6], t) ^ z[5];
        z[5] = test_msc_rs_mul(poly[5], t) ^ z[4];
        z[4] = test_msc_rs_mul(poly[4], t) ^ z[3];
        z[3] = test_msc_rs_mul(poly[3], t) ^ z[2];
        z[2] = test_msc_rs_mul(poly[2], t) ^ z[1];
        z[1] = test_msc_rs_mul(poly[1], t) ^ z[0];
        z[0] = test_msc_rs_mul(poly[0], t);
    }

    for (i = 0u; i < 10u; ++i) {
        parity[i] = z[9u - i];
    }
}

static uint16_t test_msc_dabplus_firecode(const uint8_t *data) {
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

static void test_msc_build_dabplus_cifs(uint16_t cif_bytes,
                                        uint8_t *data1,
                                        uint8_t cifs[DAB_MSC_DABPLUS_COLUMNS_PER_SUPERFRAME][DAB_MSC_MAX_LOGICAL_FRAME_BYTES_PER_CIF]) {
    const uint16_t row_count = (uint16_t)(cif_bytes / DAB_MSC_DABPLUS_RESYNC_COLUMNS);
    uint32_t row;

    for (row = 0u; row < row_count; ++row) {
        uint8_t payload_245[245] = {0};
        uint8_t parity[10];

        for (uint32_t col = 0u; col < DAB_MSC_DABPLUS_COLUMNS_PER_RS_PAYLOAD; ++col) {
            payload_245[135u + col] = data1[(col * row_count) + row];
        }
        test_msc_rs_encode_row_245_10(payload_245, parity);
        for (uint32_t col = 0u; col < DAB_MSC_DABPLUS_COLUMNS_PER_RS_PAYLOAD; ++col) {
            const uint32_t cif_index = col / DAB_MSC_DABPLUS_RESYNC_COLUMNS;
            const uint32_t cif_offset = (col % DAB_MSC_DABPLUS_RESYNC_COLUMNS) * row_count;

            cifs[cif_index][cif_offset + row] = payload_245[135u + col];
        }
        for (uint32_t col = 0u; col < 10u; ++col) {
            const uint32_t rs_col = DAB_MSC_DABPLUS_COLUMNS_PER_RS_PAYLOAD + col;
            const uint32_t cif_index = rs_col / DAB_MSC_DABPLUS_RESYNC_COLUMNS;
            const uint32_t cif_offset = (rs_col % DAB_MSC_DABPLUS_RESYNC_COLUMNS) * row_count;

            cifs[cif_index][cif_offset + row] = parity[col];
        }
    }
}

void test_msc_process_frame_tracks_frame_metadata(void) {
    dab_msc_state_t state;
    int8_t msc[DAB_TFRAME_MSC_SOFTBITS] = {0};

    msc[0] = -7;
    msc[DAB_TFRAME_MSC_SOFTBITS - 1u] = 55;

    TEST_ASSERT(dab_msc_init(&state) == 0);
    TEST_ASSERT(dab_msc_process_frame(&state, 54321u, msc, DAB_TFRAME_MSC_SOFTBITS) == 0);
    TEST_ASSERT(state.frames_seen == 1u);
    TEST_ASSERT(state.last_frame_start_sample == 54321u);
    TEST_ASSERT(state.first_softbit == -7);
    TEST_ASSERT(state.last_softbit == 55);
}

void test_msc_build_layout_resolves_uep_subchannels_and_service_mappings(void) {
    dab_fic_database_t db = {0};
    dab_msc_layout_t layout;

    db.subchannels[3].valid = 1u;
    db.subchannels[3].subchid = 3u;
    db.subchannels[3].start_cu = 100u;
    db.subchannels[3].protection_kind = DAB_FIC_PROTECTION_UEP;
    db.subchannels[3].table_switch = 0u;
    db.subchannels[3].protection_index = 17u;

    db.service_count = 1u;
    db.services[0].is_programme = 1u;
    db.services[0].sid = 0x1234u;
    db.services[0].component_count = 1u;
    db.services[0].components[0].used = 1u;
    db.services[0].components[0].tmid = 0u;
    db.services[0].components[0].sc_type = 5u;
    db.services[0].components[0].subchid = 3u;
    db.services[0].components[0].has_subchid = 1u;
    db.services[0].components[0].primary = 1u;

    TEST_ASSERT(dab_msc_build_layout(&layout, &db) == 0);
    TEST_ASSERT(layout.subchannel_count == 1u);
    TEST_ASSERT(layout.subchannels[3].valid == 1u);
    TEST_ASSERT(layout.subchannels[3].start_cu == 100u);
    TEST_ASSERT(layout.subchannels[3].size_cu == 58u);
    TEST_ASSERT(layout.subchannels[3].bitrate_kbps == 64u);
    TEST_ASSERT(layout.subchannels[3].profile == DAB_MSC_PROFILE_UEP);
    TEST_ASSERT(layout.subchannels[3].protection_level == 2u);
    TEST_ASSERT(layout.subchannels[3].softbits_per_cif == (58u * DAB_MSC_SOFTBITS_PER_CU));
    TEST_ASSERT(layout.component_count == 1u);
    TEST_ASSERT(layout.components[0].sid == 0x1234u);
    TEST_ASSERT(layout.components[0].subchid == 3u);
    TEST_ASSERT(layout.components[0].primary == 1u);
}

void test_msc_get_subchannel_softbits_returns_expected_cif_slice(void) {
    dab_msc_subchannel_t subchannel = {
        .valid = 1u,
        .subchid = 7u,
        .start_cu = 10u,
        .size_cu = 12u,
        .bitrate_kbps = 8u,
        .profile = DAB_MSC_PROFILE_EEP_A,
        .protection_level = 1u,
        .protection_option = 0u,
        .cif_count = DAB_MSC_CIFS_PER_FRAME,
        .softbits_per_cif = 12u * DAB_MSC_SOFTBITS_PER_CU
    };
    int8_t msc[DAB_TFRAME_MSC_SOFTBITS];
    const int8_t *slice = 0;
    uint32_t slice_count = 0u;
    uint32_t i;
    uint32_t expected_offset = (2u * DAB_MSC_SOFTBITS_PER_CIF) + (10u * DAB_MSC_SOFTBITS_PER_CU);

    for (i = 0u; i < DAB_TFRAME_MSC_SOFTBITS; ++i) {
        msc[i] = (int8_t)((i % 127u) - 63);
    }

    TEST_ASSERT(dab_msc_get_subchannel_softbits(msc, DAB_TFRAME_MSC_SOFTBITS, &subchannel, 2u, &slice, &slice_count) == 0);
    TEST_ASSERT(slice == &msc[expected_offset]);
    TEST_ASSERT(slice_count == (12u * DAB_MSC_SOFTBITS_PER_CU));
    TEST_ASSERT(slice[0] == msc[expected_offset]);
    TEST_ASSERT(slice[slice_count - 1u] == msc[expected_offset + slice_count - 1u]);
}

void test_msc_process_frame_with_fic_populates_layout(void) {
    dab_msc_state_t state;
    dab_fic_database_t db = {0};
    int8_t msc[DAB_TFRAME_MSC_SOFTBITS] = {0};

    db.subchannels[1].valid = 1u;
    db.subchannels[1].subchid = 1u;
    db.subchannels[1].start_cu = 12u;
    db.subchannels[1].size_cu = 12u;
    db.subchannels[1].protection_kind = DAB_FIC_PROTECTION_EEP;
    db.subchannels[1].protection_option = 0u;
    db.subchannels[1].protection_level = 0u;
    db.fig0_ext0_valid = 1u;
    db.cif_count = 100u;

    TEST_ASSERT(dab_msc_init(&state) == 0);
    TEST_ASSERT(dab_msc_process_frame_with_fic(&state, 777u, msc, DAB_TFRAME_MSC_SOFTBITS, &db) == 0);
    TEST_ASSERT(state.frames_seen == 1u);
    TEST_ASSERT(state.layout.subchannel_count == 1u);
    TEST_ASSERT(state.layout.subchannels[1].bitrate_kbps == 8u);
    TEST_ASSERT(state.layout.subchannels[1].profile == DAB_MSC_PROFILE_EEP_A);
    TEST_ASSERT(state.frame_view.subchannel_count == 1u);
    TEST_ASSERT(state.frame_view.subchannels[1].logical_frame_bits == 192u);
    TEST_ASSERT(state.frame_view.subchannels[1].coded_bits_per_cif == (12u * DAB_MSC_SOFTBITS_PER_CU));
    TEST_ASSERT(state.frame_view.subchannels[1].cif_softbit_offset[0] == (12u * DAB_MSC_SOFTBITS_PER_CU));
}

void test_msc_build_frame_view_tracks_component_offsets(void) {
    dab_fic_database_t db = {0};
    dab_msc_layout_t layout;
    dab_msc_frame_view_t frame_view;

    db.subchannels[3].valid = 1u;
    db.subchannels[3].subchid = 3u;
    db.subchannels[3].start_cu = 100u;
    db.subchannels[3].protection_kind = DAB_FIC_PROTECTION_UEP;
    db.subchannels[3].table_switch = 0u;
    db.subchannels[3].protection_index = 17u;

    db.service_count = 1u;
    db.services[0].sid = 0x2233u;
    db.services[0].component_count = 1u;
    db.services[0].components[0].used = 1u;
    db.services[0].components[0].tmid = 0u;
    db.services[0].components[0].sc_type = 0u;
    db.services[0].components[0].subchid = 3u;
    db.services[0].components[0].has_subchid = 1u;
    db.services[0].components[0].primary = 1u;

    TEST_ASSERT(dab_msc_build_layout(&layout, &db) == 0);
    TEST_ASSERT(dab_msc_build_frame_view(&frame_view, &layout, DAB_TFRAME_MSC_SOFTBITS) == 0);
    TEST_ASSERT(frame_view.subchannel_count == 1u);
    TEST_ASSERT(frame_view.component_count == 1u);
    TEST_ASSERT(frame_view.subchannels[3].logical_frame_bits == (64u * 24u));
    TEST_ASSERT(frame_view.subchannels[3].coded_bits_per_frame == (58u * DAB_MSC_SOFTBITS_PER_CU * DAB_MSC_CIFS_PER_FRAME));
    TEST_ASSERT(frame_view.components[0].sid == 0x2233u);
    TEST_ASSERT(frame_view.components[0].subchid == 3u);
    TEST_ASSERT(frame_view.components[0].logical_frame_bits == frame_view.subchannels[3].logical_frame_bits);
    TEST_ASSERT(frame_view.components[0].cif_softbit_offset[0] == (100u * DAB_MSC_SOFTBITS_PER_CU));
    TEST_ASSERT(frame_view.components[0].cif_softbit_offset[3] ==
                (3u * DAB_MSC_SOFTBITS_PER_CIF) + (100u * DAB_MSC_SOFTBITS_PER_CU));
}

void test_msc_prepare_decoder_config_resolves_uep_and_eep_segments(void) {
    dab_msc_subchannel_t uep = {
        .valid = 1u,
        .subchid = 3u,
        .size_cu = 58u,
        .bitrate_kbps = 64u,
        .profile = DAB_MSC_PROFILE_UEP,
        .protection_level = 2u,
        .protection_index = 17u
    };
    dab_msc_subchannel_t eep = {
        .valid = 1u,
        .subchid = 1u,
        .size_cu = 12u,
        .bitrate_kbps = 8u,
        .profile = DAB_MSC_PROFILE_EEP_A,
        .protection_level = 1u
    };
    dab_msc_decoder_config_t config;

    TEST_ASSERT(dab_msc_prepare_decoder_config(&config, &uep) == 0);
    TEST_ASSERT(config.framebits == 1536u);
    TEST_ASSERT(config.conv_len == (58u * DAB_MSC_SOFTBITS_PER_CU));
    TEST_ASSERT(config.segment_count == 5u);
    TEST_ASSERT(config.padding_bits == 8u);
    TEST_ASSERT(config.segments[0].input_bits == (6u * 128u));
    TEST_ASSERT(config.segments[0].code_index == 23u);
    TEST_ASSERT(config.segments[4].input_bits == 24u);
    TEST_ASSERT(config.segments[4].code_index == 25u);

    TEST_ASSERT(dab_msc_prepare_decoder_config(&config, &eep) == 0);
    TEST_ASSERT(config.framebits == 192u);
    TEST_ASSERT(config.conv_len == (12u * DAB_MSC_SOFTBITS_PER_CU));
    TEST_ASSERT(config.segment_count == 3u);
    TEST_ASSERT(config.padding_bits == 0u);
    TEST_ASSERT(config.segments[0].input_bits == ((6u * 1u - 3u) * 128u));
    TEST_ASSERT(config.segments[0].code_index == 24u);
    TEST_ASSERT(config.segments[1].code_index == 23u);
    TEST_ASSERT(config.segments[2].code_index == 25u);
}

void test_msc_convolutional_deinterleave_applies_delay_line(void) {
    dab_msc_deinterleaver_t state;
    int8_t in = 0;
    int8_t out = 0;
    uint32_t i;

    dab_msc_deinterleaver_reset(&state);
    for (i = 0u; i < 15u; ++i) {
        in = (int8_t)(i + 1u);
        TEST_ASSERT(dab_msc_convolutional_deinterleave(&state, &in, 1u, &out, 1u) == 0);
        TEST_ASSERT(out == 0);
    }

    in = 16;
    TEST_ASSERT(dab_msc_convolutional_deinterleave(&state, &in, 1u, &out, 1u) == 0);
    TEST_ASSERT(out == 1);
}

void test_msc_convolutional_deinterleave_matches_shift_register(void) {
    enum { TEST_BITS = 64, TEST_STEPS = 40 };
    static const uint8_t delays[DAB_MSC_CONV_DEINTERLEAVER_DEPTH] = {
        15u, 7u, 11u, 3u, 13u, 5u, 9u, 1u, 14u, 6u, 10u, 2u, 12u, 4u, 8u, 0u
    };
    dab_msc_deinterleaver_t state;
    int8_t reference[TEST_BITS][DAB_MSC_CONV_DEINTERLEAVER_DEPTH] = {{0}};
    int8_t input[TEST_BITS];
    int8_t output[TEST_BITS];
    uint32_t step;

    dab_msc_deinterleaver_reset(&state);
    for (step = 0u; step < TEST_STEPS; ++step) {
        uint32_t bit;

        for (bit = 0u; bit < TEST_BITS; ++bit) {
            input[bit] = (int8_t)(((step * 37u) + (bit * 11u)) & 0x7Fu);
        }
        TEST_ASSERT(dab_msc_convolutional_deinterleave(&state,
                                                       input,
                                                       TEST_BITS,
                                                       output,
                                                       TEST_BITS) == 0);

        for (bit = 0u; bit < TEST_BITS; ++bit) {
            const uint32_t delay = delays[bit & 0x0Fu];
            uint32_t age;
            int8_t history_value = 0;

            for (age = DAB_MSC_CONV_DEINTERLEAVER_DEPTH - 1u; age > 0u; --age) {
                reference[bit][age] = reference[bit][age - 1u];
            }
            reference[bit][0] = input[bit];
            TEST_ASSERT(output[bit] == reference[bit][delay]);
            for (age = 0u; age < DAB_MSC_CONV_DEINTERLEAVER_DEPTH; ++age) {
                TEST_ASSERT(dab_msc_deinterleaver_get_history(&state,
                                                              bit,
                                                              age,
                                                              &history_value) == 0);
                TEST_ASSERT(history_value == reference[bit][age]);
            }
        }
    }
}

void test_msc_decode_deinterleaved_cif_decodes_synthetic_eep_payload(void) {
    dab_msc_subchannel_t eep = {
        .valid = 1u,
        .subchid = 1u,
        .size_cu = 12u,
        .bitrate_kbps = 8u,
        .profile = DAB_MSC_PROFILE_EEP_A,
        .protection_level = 1u
    };
    dab_msc_decoder_config_t config;
    int8_t softbits[12u * DAB_MSC_SOFTBITS_PER_CU];
    uint8_t payload[24];
    uint8_t decoded[24] = {0};
    static const uint8_t expected[24] = {
        0xE3u, 0x47u, 0x1Eu, 0x86u, 0x7Eu, 0x80u, 0x02u, 0xD8u,
        0x56u, 0x29u, 0xB3u, 0xE8u, 0x4Au, 0xACu, 0xEDu, 0x75u,
        0x0Cu, 0xB0u, 0xE9u, 0xB2u, 0x91u, 0xD2u, 0x48u, 0xD0u
    };
    uint32_t i;

    TEST_ASSERT(dab_msc_prepare_decoder_config(&config, &eep) == 0);
    for (i = 0u; i < sizeof(payload); ++i) {
        payload[i] = (uint8_t)(0x20u + i);
    }

    test_msc_build_deinterleaved_cif(payload, &config, softbits);
    TEST_ASSERT(dab_msc_decode_deinterleaved_cif(softbits, config.conv_len, &config, decoded, sizeof(decoded)) == 0);
    TEST_ASSERT(memcmp(decoded, expected, sizeof(expected)) == 0);
}

void test_msc_sync_runtimes_prepares_and_resets_decoder_state(void) {
    dab_msc_layout_t layout = {0};
    dab_msc_subchannel_runtime_t runtimes[DAB_FIC_MAX_SUBCHANNELS] = {0};
    dab_msc_decoder_workspace_t *workspace;

    layout.subchannels[1].valid = 1u;
    layout.subchannels[1].subchid = 1u;
    layout.subchannels[1].size_cu = 12u;
    layout.subchannels[1].bitrate_kbps = 8u;
    layout.subchannels[1].profile = DAB_MSC_PROFILE_EEP_A;
    layout.subchannels[1].protection_level = 1u;
    layout.subchannel_count = 1u;

    TEST_ASSERT(dab_msc_sync_runtimes(runtimes, &layout) == 0);
    TEST_ASSERT(runtimes[1].valid == 1u);
    TEST_ASSERT(runtimes[1].config.framebits == 192u);
    TEST_ASSERT(runtimes[1].config.conv_len == (12u * DAB_MSC_SOFTBITS_PER_CU));
    TEST_ASSERT(runtimes[1].deinterleaver != 0);
    TEST_ASSERT(runtimes[1].workspace != 0);
    workspace = runtimes[1].workspace;

    runtimes[1].deinterleaver->regs[0] = 55;
    TEST_ASSERT(dab_msc_sync_runtimes(runtimes, &layout) == 0);
    TEST_ASSERT(runtimes[1].deinterleaver->regs[0] == 55);
    TEST_ASSERT(runtimes[1].workspace == workspace);

    layout.subchannels[1].size_cu = 8u;
    layout.subchannels[1].bitrate_kbps = 6u;
    layout.subchannels[1].protection_level = 1u;
    TEST_ASSERT(dab_msc_sync_runtimes(runtimes, &layout) == 0);
    TEST_ASSERT(runtimes[1].valid == 0u);

    memset(&layout, 0, sizeof(layout));
    TEST_ASSERT(dab_msc_sync_runtimes(runtimes, &layout) == 0);
    TEST_ASSERT(runtimes[1].valid == 0u);
}

void test_msc_service_filter_decodes_only_selected_audio_subchannel(void) {
    dab_msc_state_t state;
    dab_fic_database_t db = {0};
    int8_t msc[DAB_TFRAME_MSC_SOFTBITS] = {0};
    uint32_t index;

    for (index = 1u; index <= 2u; ++index) {
        db.subchannels[index].valid = 1u;
        db.subchannels[index].subchid = (uint8_t)index;
        db.subchannels[index].start_cu = (uint16_t)(index * 12u);
        db.subchannels[index].size_cu = 12u;
        db.subchannels[index].protection_kind = DAB_FIC_PROTECTION_EEP;
        db.subchannels[index].protection_option = 0u;
        db.subchannels[index].protection_level = 0u;

        db.services[index - 1u].sid = 0x4000u + index;
        db.services[index - 1u].component_count = 1u;
        db.services[index - 1u].components[0].used = 1u;
        db.services[index - 1u].components[0].tmid = 0u;
        db.services[index - 1u].components[0].sc_type = 0x3fu;
        db.services[index - 1u].components[0].subchid = (uint8_t)index;
        db.services[index - 1u].components[0].has_subchid = 1u;
        db.services[index - 1u].components[0].primary = 1u;
    }
    db.service_count = 2u;

    TEST_ASSERT(dab_msc_init(&state) == 0);
    dab_msc_set_service_filter(&state, 0x4002u);
    TEST_ASSERT(dab_msc_process_frame_with_fic(&state, 100u, msc, DAB_TFRAME_MSC_SOFTBITS, &db) == 0);
    TEST_ASSERT(state.runtimes[1].valid == 0u);
    TEST_ASSERT(state.runtimes[2].valid == 1u);
    TEST_ASSERT(state.decoded_subchannels[1].valid == 0u);
    TEST_ASSERT(state.decoded_subchannels[2].valid == 1u);

    dab_msc_reset(&state);
    TEST_ASSERT(state.service_filter_enabled == 1u);
    TEST_ASSERT(state.service_filter_sid == 0x4002u);
    dab_msc_clear_service_filter(&state);
    TEST_ASSERT(state.service_filter_enabled == 0u);
}

void test_msc_process_frame_with_fic_tracks_decoded_component_bindings(void) {
    dab_msc_state_t state;
    dab_fic_database_t db = {0};
    int8_t msc[DAB_TFRAME_MSC_SOFTBITS] = {0};
    dab_msc_deinterleaver_t *legacy_deinterleaver;
    uint8_t legacy_bytes[DAB_MSC_CIFS_PER_FRAME][DAB_MSC_MAX_LOGICAL_FRAME_BYTES_PER_CIF] = {{0}};
    uint32_t cif_index;

    legacy_deinterleaver = (dab_msc_deinterleaver_t *)calloc(1u, sizeof(*legacy_deinterleaver));
    TEST_ASSERT(legacy_deinterleaver != 0);

    db.subchannels[1].valid = 1u;
    db.subchannels[1].subchid = 1u;
    db.subchannels[1].start_cu = 12u;
    db.subchannels[1].size_cu = 12u;
    db.subchannels[1].protection_kind = DAB_FIC_PROTECTION_EEP;
    db.subchannels[1].protection_option = 0u;
    db.subchannels[1].protection_level = 0u;
    db.fig0_ext0_valid = 1u;
    db.cif_count = 100u;

    db.service_count = 1u;
    db.services[0].sid = 0x3456u;
    db.services[0].component_count = 1u;
    db.services[0].components[0].used = 1u;
    db.services[0].components[0].tmid = 0u;
    db.services[0].components[0].sc_type = 0u;
    db.services[0].components[0].subchid = 1u;
    db.services[0].components[0].has_subchid = 1u;
    db.services[0].components[0].primary = 1u;

    TEST_ASSERT(dab_msc_init(&state) == 0);
    TEST_ASSERT(dab_msc_process_frame_with_fic(&state, 123u, msc, DAB_TFRAME_MSC_SOFTBITS, &db) == 0);
    TEST_ASSERT(state.runtimes[1].valid == 1u);
    TEST_ASSERT(state.decoded_subchannels[1].valid == 1u);
    TEST_ASSERT(state.decoded_subchannels[1].subchid == 1u);
    TEST_ASSERT(state.decoded_component_count == 1u);
    TEST_ASSERT(state.decoded_components[0].used == 1u);
    TEST_ASSERT(state.decoded_components[0].sid == 0x3456u);
    TEST_ASSERT(state.decoded_components[0].subchid == 1u);
    TEST_ASSERT(state.decoded_components[0].tmid == 0u);
    TEST_ASSERT(state.runtimes[1].deinterleaver_cifs_seen ==
                DAB_MSC_CIFS_PER_FRAME);
    TEST_ASSERT(state.runtimes[1].decoded_cif_attempts ==
                DAB_MSC_CIFS_PER_FRAME);
    TEST_ASSERT(state.runtimes[1].decoded_cif_successes ==
                DAB_MSC_CIFS_PER_FRAME);
    TEST_ASSERT(state.runtimes[1].decoded_cif_failures == 0u);
    TEST_ASSERT(state.runtimes[1].decoded_output_bytes ==
                4u * (state.runtimes[1].config.framebits / 8u));
    TEST_ASSERT(state.cif_counter_valid == 1u);
    TEST_ASSERT(state.last_cif_count == 100u);
    TEST_ASSERT(state.cif_discontinuity_count == 0u);

    for (cif_index = 0u; cif_index < DAB_MSC_CIFS_PER_FRAME; ++cif_index) {
        TEST_ASSERT(dab_msc_decode_subchannel_cif(msc,
                                                  DAB_TFRAME_MSC_SOFTBITS,
                                                  &state.frame_view.subchannels[1],
                                                  &state.runtimes[1].config,
                                                  legacy_deinterleaver,
                                                  cif_index,
                                                  legacy_bytes[cif_index],
                                                  state.decoded_subchannels[1].byte_count[cif_index]) == 0);
        TEST_ASSERT(memcmp(legacy_bytes[cif_index],
                           state.decoded_subchannels[1].data[cif_index],
                           state.decoded_subchannels[1].byte_count[cif_index]) == 0);
    }
    db.services[0].components[0].tmid = 1u;
    db.cif_count = 104u;
    TEST_ASSERT(dab_msc_process_frame_with_fic(&state,
                                                124u,
                                                msc,
                                                DAB_TFRAME_MSC_SOFTBITS,
                                                &db) == 0);
    TEST_ASSERT(state.cif_discontinuity_count == 0u);
    db.cif_count = 120u;
    TEST_ASSERT(dab_msc_process_frame_with_fic(&state,
                                                125u,
                                                msc,
                                                DAB_TFRAME_MSC_SOFTBITS,
                                                &db) == 0);
    TEST_ASSERT(state.cif_discontinuity_count == 1u);
    free(legacy_deinterleaver);
    dab_msc_reset(&state);
}

void test_msc_component_kind_classifies_transport_mode(void) {
    TEST_ASSERT(dab_msc_component_kind(0u) == DAB_MSC_COMPONENT_STREAM_AUDIO);
    TEST_ASSERT(dab_msc_component_kind(1u) == DAB_MSC_COMPONENT_STREAM_DATA);
    TEST_ASSERT(dab_msc_component_kind(3u) == DAB_MSC_COMPONENT_PACKET_MODE);
    TEST_ASSERT(dab_msc_component_kind(2u) == DAB_MSC_COMPONENT_UNKNOWN);
}

void test_msc_find_primary_component_payload_exposes_decoded_cifs(void) {
    dab_msc_state_t state = {0};
    dab_msc_component_payload_t payload;
    uint8_t *slot0;
    uint8_t *slot1;

    state.decoded_component_count = 2u;
    state.decoded_components[0].used = 1u;
    state.decoded_components[0].sid = 0x1234u;
    state.decoded_components[0].component_index = 0u;
    state.decoded_components[0].subchid = 5u;
    state.decoded_components[0].tmid = 1u;
    state.decoded_components[0].primary = 0u;

    state.decoded_components[1].used = 1u;
    state.decoded_components[1].sid = 0x1234u;
    state.decoded_components[1].component_index = 1u;
    state.decoded_components[1].subchid = 7u;
    state.decoded_components[1].tmid = 0u;
    state.decoded_components[1].sc_type = 0x3Fu;
    state.decoded_components[1].primary = 1u;

    state.decoded_subchannels[7].valid = 1u;
    state.decoded_subchannels[7].subchid = 7u;
    state.decoded_subchannels[7].cif_valid[0] = 1u;
    state.decoded_subchannels[7].cif_valid[1] = 1u;
    state.decoded_subchannels[7].byte_count[0] = 24u;
    state.decoded_subchannels[7].byte_count[1] = 24u;
    slot0 = state.decoded_subchannels[7].data[0];
    slot1 = state.decoded_subchannels[7].data[1];
    slot0[0] = 0x11u;
    slot1[0] = 0x22u;

    TEST_ASSERT(dab_msc_find_decoded_component(&state, 0x1234u, 1u) == &state.decoded_components[1]);
    TEST_ASSERT(dab_msc_find_primary_decoded_component(&state, 0x1234u) == &state.decoded_components[1]);
    TEST_ASSERT(dab_msc_get_primary_component_payload(&state, 0x1234u, &payload) == 0);
    TEST_ASSERT(payload.valid == 1u);
    TEST_ASSERT(payload.sid == 0x1234u);
    TEST_ASSERT(payload.subchid == 7u);
    TEST_ASSERT(payload.kind == DAB_MSC_COMPONENT_STREAM_AUDIO);
    TEST_ASSERT(payload.cif_valid[0] == 1u);
    TEST_ASSERT(payload.cif_valid[1] == 1u);
    TEST_ASSERT(payload.byte_count[0] == 24u);
    TEST_ASSERT(payload.byte_count[1] == 24u);
    TEST_ASSERT(payload.data[0] == slot0);
    TEST_ASSERT(payload.data[1] == slot1);
    TEST_ASSERT(payload.data[0][0] == 0x11u);
    TEST_ASSERT(payload.data[1][0] == 0x22u);
}

void test_msc_ingest_component_payload_builds_dabplus_superframe(void) {
    dab_msc_state_t state;
    dab_msc_layout_t layout = {0};
    dab_msc_component_payload_t payload;
    dab_msc_dabplus_superframe_t superframe;
    dab_msc_dabplus_debug_t debug;
    uint8_t data1[24u * DAB_MSC_DABPLUS_COLUMNS_PER_RS_PAYLOAD];
    uint8_t cifs[DAB_MSC_DABPLUS_COLUMNS_PER_SUPERFRAME][DAB_MSC_MAX_LOGICAL_FRAME_BYTES_PER_CIF] = {{0}};
    uint16_t firecode;
    uint32_t i;

    memset(data1, 0, sizeof(data1));
    for (i = 0u; i < sizeof(data1); ++i) {
        data1[i] = (uint8_t)(0x30u + (i % 97u));
    }
    data1[2] = 0x60u;
    data1[3] = 0x19u;
    data1[4] = 0x03u;
    data1[5] = 0x20u;
    data1[6] = 0x4Bu;
    data1[7] = 0x00u;
    firecode = test_msc_dabplus_firecode(&data1[2]);
    data1[0] = (uint8_t)(firecode >> 8u);
    data1[1] = (uint8_t)(firecode & 0xFFu);
    test_msc_build_dabplus_cifs(24u, data1, cifs);

    TEST_ASSERT(dab_msc_init(&state) == 0);
    layout.subchannels[1].valid = 1u;
    layout.subchannels[1].subchid = 1u;
    layout.subchannels[1].size_cu = 12u;
    layout.subchannels[1].bitrate_kbps = 8u;
    layout.subchannels[1].profile = DAB_MSC_PROFILE_EEP_A;
    layout.subchannels[1].protection_level = 1u;
    layout.subchannel_count = 1u;
    TEST_ASSERT(dab_msc_sync_runtimes(state.runtimes, &layout) == 0);

    state.decoded_component_count = 1u;
    state.decoded_components[0].used = 1u;
    state.decoded_components[0].sid = 0x5555u;
    state.decoded_components[0].subchid = 1u;
    state.decoded_components[0].tmid = 0u;
    state.decoded_components[0].primary = 1u;

    for (i = 0u; i < (DAB_MSC_DABPLUS_COLUMNS_PER_SUPERFRAME / DAB_MSC_DABPLUS_RESYNC_COLUMNS); i += DAB_MSC_CIFS_PER_FRAME) {
        payload = (dab_msc_component_payload_t){0};
        payload.valid = 1u;
        payload.sid = 0x5555u;
        payload.subchid = 1u;
        payload.tmid = 0u;
        payload.primary = 1u;
        payload.kind = DAB_MSC_COMPONENT_STREAM_AUDIO;
        for (uint32_t cif_index = 0u; cif_index < DAB_MSC_CIFS_PER_FRAME; ++cif_index) {
            const uint32_t column = i + cif_index;

            if (column >= (DAB_MSC_DABPLUS_COLUMNS_PER_SUPERFRAME / DAB_MSC_DABPLUS_RESYNC_COLUMNS)) {
                break;
            }

            payload.cif_valid[cif_index] = 1u;
            payload.byte_count[cif_index] = 24u * DAB_MSC_DABPLUS_RESYNC_COLUMNS;
            payload.data[cif_index] = cifs[column];
        }
        TEST_ASSERT(dab_msc_ingest_component_payload(&state, &state.decoded_components[0], &payload) == 0);
    }

    TEST_ASSERT(dab_msc_get_primary_dabplus_superframe(&state, 0x5555u, &superframe) == 0);
    TEST_ASSERT(superframe.valid == 1u);
    TEST_ASSERT(superframe.synchronized == 1u);
    TEST_ASSERT(superframe.cif_bytes == (24u * DAB_MSC_DABPLUS_RESYNC_COLUMNS));
    TEST_ASSERT(superframe.superframe_bytes == sizeof(data1));
    TEST_ASSERT(superframe.au_count == 3u);
    TEST_ASSERT(superframe.dac_rate == 1u);
    TEST_ASSERT(superframe.sbr_flag == 1u);
    TEST_ASSERT(superframe.au_start[0] == 6u);
    TEST_ASSERT(superframe.au_start[1] == 400u);
    TEST_ASSERT(superframe.au_start[2] == 800u);
    TEST_ASSERT(superframe.au_length[0] == 392u);
    TEST_ASSERT(superframe.au_length[1] == 398u);
    TEST_ASSERT(superframe.au_length[2] == (sizeof(data1) - 800u - 2u));
    TEST_ASSERT(superframe.superframe_data != 0);
    TEST_ASSERT(superframe.superframe_data[0] != 0u);
    TEST_ASSERT(superframe.superframe_data[1] != 0u);
    TEST_ASSERT(dab_msc_get_primary_dabplus_debug(&state, 0x5555u, &debug) == 0);
    TEST_ASSERT(debug.valid == 1u);
    TEST_ASSERT(debug.au_crc_checks_total == 3u);
    TEST_ASSERT(debug.au_crc_errors_last_superframe == 3u);
    TEST_ASSERT(debug.au_crc_errors_total == 3u);
}

void test_msc_build_layout_and_payload_preserve_packet_metadata(void) {
    dab_fic_database_t db = {0};
    dab_msc_state_t state;
    dab_msc_component_payload_t payload;
    int8_t msc[DAB_TFRAME_MSC_SOFTBITS] = {0};

    db.subchannels[1].valid = 1u;
    db.subchannels[1].subchid = 1u;
    db.subchannels[1].start_cu = 12u;
    db.subchannels[1].size_cu = 12u;
    db.subchannels[1].protection_kind = DAB_FIC_PROTECTION_EEP;
    db.subchannels[1].protection_option = 0u;
    db.subchannels[1].protection_level = 0u;

    db.service_count = 1u;
    db.services[0].sid = 0x3456u;
    db.services[0].component_count = 1u;
    db.services[0].components[0].used = 1u;
    db.services[0].components[0].tmid = 3u;
    db.services[0].components[0].sc_type = 0x15u;
    db.services[0].components[0].scid = 0x456u;
    db.services[0].components[0].subchid = 1u;
    db.services[0].components[0].has_subchid = 1u;
    db.services[0].components[0].packet_addr = 0x155u;
    db.services[0].components[0].has_packet_addr = 1u;
    db.services[0].components[0].dg_flag = 1u;
    db.services[0].components[0].caorg = 0xBEEFu;
    db.services[0].components[0].has_caorg = 1u;

    TEST_ASSERT(dab_msc_init(&state) == 0);
    TEST_ASSERT(dab_msc_process_frame_with_fic(&state, 456u, msc, DAB_TFRAME_MSC_SOFTBITS, &db) == 0);
    TEST_ASSERT(state.layout.component_count == 1u);
    TEST_ASSERT(state.layout.components[0].tmid == 3u);
    TEST_ASSERT(state.layout.components[0].packet_addr == 0x155u);
    TEST_ASSERT(state.layout.components[0].has_packet_addr == 1u);
    TEST_ASSERT(state.decoded_component_count == 1u);
    TEST_ASSERT(state.decoded_components[0].tmid == 3u);
    TEST_ASSERT(state.decoded_components[0].packet_addr == 0x155u);
    TEST_ASSERT(state.decoded_components[0].dg_flag == 1u);
    TEST_ASSERT(state.decoded_components[0].caorg == 0xBEEFu);
    TEST_ASSERT(dab_msc_get_primary_component_payload(&state, 0x3456u, &payload) == 0);
    TEST_ASSERT(payload.kind == DAB_MSC_COMPONENT_PACKET_MODE);
    TEST_ASSERT(payload.packet_addr == 0x155u);
    TEST_ASSERT(payload.has_packet_addr == 1u);
    TEST_ASSERT(payload.dg_flag == 1u);
    TEST_ASSERT(payload.caorg == 0xBEEFu);
    TEST_ASSERT(payload.has_caorg == 1u);
}
