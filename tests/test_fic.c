#include "dab/core/fic.h"

#include <string.h>

#include "test_support.h"

#define TEST_FIC_SOFTBIT_ONE 96
#define TEST_FIC_SOFTBIT_ZERO (-96)
#define TEST_FIC_TRELLIS_BITS 774u
#define TEST_FIC_INFO_BITS 768u
#define TEST_FIC_UNPUNCTURED_BITS 3096u
#define TEST_FIC_DEPARTITIONED_BITS 2304u

static uint16_t test_fic_crc16_ccitt(const uint8_t *buf, size_t len);

static void test_fic_build_fib_with_payload(uint8_t fib[DAB_FIC_BYTES_PER_FIB],
                                            const uint8_t *payload,
                                            uint32_t payload_size) {
    uint16_t crc;

    memset(fib, 0xFF, DAB_FIC_BYTES_PER_FIB - 2u);
    if (payload != 0 && payload_size > 0u) {
        memcpy(fib, payload, payload_size);
    }
    crc = test_fic_crc16_ccitt(fib, DAB_FIC_BYTES_PER_FIB - 2u);
    fib[30] = (uint8_t)(crc >> 8u);
    fib[31] = (uint8_t)(crc & 0xFFu);
}

static void test_fic_write_bits(uint8_t *buf,
                                uint32_t *bit_pos,
                                uint32_t value,
                                uint32_t bit_count) {
    uint32_t i;

    for (i = 0u; i < bit_count; ++i) {
        const uint32_t src_shift = bit_count - 1u - i;
        const uint32_t byte_index = (*bit_pos) >> 3u;
        const uint32_t dst_shift = 7u - ((*bit_pos) & 7u);

        if (((value >> src_shift) & 1u) != 0u) {
            buf[byte_index] |= (uint8_t)(1u << dst_shift);
        }
        (*bit_pos)++;
    }
}

static uint8_t test_fic_mother_code(uint8_t bit, uint8_t *state) {
    const uint8_t x0 = (uint8_t)((((*state) >> 5) & 1u) ^
                                 (((*state) >> 4) & 1u) ^
                                 (((*state) >> 2) & 1u) ^
                                 (((*state) >> 1) & 1u) ^
                                 (bit & 1u));
    const uint8_t x1 = (uint8_t)((((*state) >> 5) & 1u) ^
                                 (((*state) >> 2) & 1u) ^
                                 (((*state) >> 1) & 1u) ^
                                 (((*state) >> 0) & 1u) ^
                                 (bit & 1u));
    const uint8_t x2 = (uint8_t)((((*state) >> 5) & 1u) ^
                                 (((*state) >> 3) & 1u) ^
                                 (((*state) >> 0) & 1u) ^
                                 (bit & 1u));
    const uint8_t x3 = x0;
    const uint8_t code = (uint8_t)((x0 & 1u) |
                                   ((x1 & 1u) << 1u) |
                                   ((x2 & 1u) << 2u) |
                                   ((x3 & 1u) << 3u));

    *state = (uint8_t)((((*state) << 1u) & 0x3Fu) | (bit & 1u));
    return code;
}

static uint32_t test_fic_puncturing_code(uint32_t p) {
    switch (p) {
        case 15u: return 0x37777777u;
        case 16u: return 0x77777777u;
        case 25u: return 0x00333333u;
        default:  return 0u;
    }
}

static uint32_t test_fic_get_puncturing_code(uint32_t bit_addr) {
    uint32_t low = 0u;

    if (bit_addr >= low && bit_addr < low + (21u * 128u)) {
        return test_fic_puncturing_code(16u);
    }
    low += 21u * 128u;
    if (bit_addr >= low && bit_addr < low + (3u * 128u)) {
        return test_fic_puncturing_code(15u);
    }
    low += 3u * 128u;
    if (bit_addr >= low && bit_addr < low + 24u) {
        return test_fic_puncturing_code(25u);
    }
    return 0u;
}

static uint8_t test_fic_prbs_bit(int reset) {
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

static uint8_t test_fic_prbs8(uint32_t reset) {
    uint8_t value = 0u;
    uint32_t i;

    for (i = 0u; i < 8u; ++i) {
        value = (uint8_t)((value << 1u) | test_fic_prbs_bit(reset != 0u && i == 0u));
    }

    return value;
}

static uint16_t test_fic_crc16_ccitt(const uint8_t *buf, size_t len) {
    uint16_t crc = 0xFFFFu;
    size_t i;

    for (i = 0u; i < len; ++i) {
        uint32_t bit;

        crc ^= (uint16_t)buf[i] << 8u;
        for (bit = 0u; bit < 8u; ++bit) {
            if ((crc & 0x8000u) != 0u) {
                crc = (uint16_t)((crc << 1u) ^ 0x1021u);
            } else {
                crc = (uint16_t)(crc << 1u);
            }
        }
    }

    return (uint16_t)(~crc);
}

static void test_fic_build_valid_fib(uint8_t fib[DAB_FIC_BYTES_PER_FIB], uint8_t seed) {
    uint32_t i;
    uint16_t crc;

    for (i = 0u; i < DAB_FIC_BYTES_PER_FIB - 2u; ++i) {
        fib[i] = (uint8_t)(seed + (uint8_t)(3u * i));
    }

    crc = test_fic_crc16_ccitt(fib, DAB_FIC_BYTES_PER_FIB - 2u);
    fib[30] = (uint8_t)(crc >> 8u);
    fib[31] = (uint8_t)(crc & 0xFFu);
}

static void test_fic_encode_block(const uint8_t decoded_block[DAB_FIC_BYTES_PER_BLOCK],
                                  int8_t frame_softbits[DAB_TFRAME_FIC_SOFTBITS],
                                  uint32_t block_index,
                                  int msb_packed) {
    uint8_t scrambled[DAB_FIC_BYTES_PER_BLOCK];
    uint8_t input_bits[TEST_FIC_TRELLIS_BITS] = {0};
    uint8_t coded_bits[TEST_FIC_UNPUNCTURED_BITS] = {0};
    uint32_t punctured_pos = block_index * TEST_FIC_DEPARTITIONED_BITS;
    uint32_t byte_index;
    uint32_t bit_index;
    uint32_t out_pos;
    uint8_t encoder_state = 0u;

    for (byte_index = 0u; byte_index < DAB_FIC_BYTES_PER_BLOCK; ++byte_index) {
        scrambled[byte_index] = (uint8_t)(decoded_block[byte_index] ^ test_fic_prbs8(byte_index == 0u));
        for (bit_index = 0u; bit_index < 8u; ++bit_index) {
            const uint32_t src_bit = msb_packed != 0 ? (7u - bit_index) : bit_index;
            input_bits[(byte_index * 8u) + bit_index] = (uint8_t)((scrambled[byte_index] >> src_bit) & 1u);
        }
    }

    for (bit_index = 0u; bit_index < TEST_FIC_TRELLIS_BITS; ++bit_index) {
        const uint8_t code = test_fic_mother_code(input_bits[bit_index], &encoder_state);
        uint32_t j;
        for (j = 0u; j < 4u; ++j) {
            coded_bits[(bit_index * 4u) + j] = (uint8_t)((code >> j) & 1u);
        }
    }

    out_pos = 0u;
    while (out_pos < TEST_FIC_UNPUNCTURED_BITS) {
        const uint32_t pattern = test_fic_get_puncturing_code(out_pos);
        const uint32_t chunk_bits = (TEST_FIC_UNPUNCTURED_BITS - out_pos >= 32u)
                                        ? 32u
                                        : (TEST_FIC_UNPUNCTURED_BITS - out_pos);
        uint32_t i;

        for (i = 0u; i < chunk_bits; ++i) {
            if (((pattern >> i) & 1u) != 0u) {
                frame_softbits[punctured_pos++] = coded_bits[out_pos + i] != 0u
                                                      ? TEST_FIC_SOFTBIT_ONE
                                                      : TEST_FIC_SOFTBIT_ZERO;
            }
        }
        out_pos += chunk_bits;
    }

    TEST_ASSERT(punctured_pos == ((block_index + 1u) * TEST_FIC_DEPARTITIONED_BITS));
}

void test_fic_process_frame_decodes_valid_fibs(void) {
    dab_fic_state_t state;
    int8_t fic[DAB_TFRAME_FIC_SOFTBITS] = {0};
    uint8_t expected[DAB_FIC_TOTAL_FIBS_PER_FRAME][DAB_FIC_BYTES_PER_FIB];
    uint32_t block_index;
    uint32_t fib_index;

    TEST_ASSERT(dab_fic_init(&state) == 0);

    for (block_index = 0u; block_index < DAB_FIC_BLOCKS_PER_FRAME; ++block_index) {
        uint8_t block[DAB_FIC_BYTES_PER_BLOCK];

        for (fib_index = 0u; fib_index < DAB_FIC_FIBS_PER_BLOCK; ++fib_index) {
            const uint32_t global_index = (block_index * DAB_FIC_FIBS_PER_BLOCK) + fib_index;
            test_fic_build_valid_fib(expected[global_index], (uint8_t)(16u + global_index));
            memcpy(block + (fib_index * DAB_FIC_BYTES_PER_FIB),
                   expected[global_index],
                   DAB_FIC_BYTES_PER_FIB);
        }

        test_fic_encode_block(block, fic, block_index, 1);
    }

    TEST_ASSERT(dab_fic_process_frame(&state, 12345u, fic, DAB_TFRAME_FIC_SOFTBITS) == 0);
    TEST_ASSERT(state.frames_seen == 1u);
    TEST_ASSERT(state.last_frame_start_sample == 12345u);
    TEST_ASSERT(state.fic_blocks_decoded_last_frame == DAB_FIC_BLOCKS_PER_FRAME);
    TEST_ASSERT(state.valid_fibs_last_frame == DAB_FIC_TOTAL_FIBS_PER_FRAME);
    TEST_ASSERT(state.total_valid_fibs == DAB_FIC_TOTAL_FIBS_PER_FRAME);

    for (fib_index = 0u; fib_index < DAB_FIC_TOTAL_FIBS_PER_FRAME; ++fib_index) {
        TEST_ASSERT(state.fib_crc_ok[fib_index] == 1u);
        TEST_ASSERT(memcmp(state.decoded_fibs[fib_index],
                           expected[fib_index],
                           DAB_FIC_BYTES_PER_FIB) == 0);
    }
    dab_fic_destroy(&state);
}

void test_fic_process_frame_reports_crc_errors(void) {
    dab_fic_state_t state;
    int8_t fic[DAB_TFRAME_FIC_SOFTBITS] = {0};
    uint32_t block_index;
    uint32_t fib_index;

    TEST_ASSERT(dab_fic_init(&state) == 0);

    for (block_index = 0u; block_index < DAB_FIC_BLOCKS_PER_FRAME; ++block_index) {
        uint8_t block[DAB_FIC_BYTES_PER_BLOCK];

        for (fib_index = 0u; fib_index < DAB_FIC_FIBS_PER_BLOCK; ++fib_index) {
            uint8_t fib[DAB_FIC_BYTES_PER_FIB];
            test_fic_build_valid_fib(fib, (uint8_t)(32u + fib_index + (block_index * 3u)));
            memcpy(block + (fib_index * DAB_FIC_BYTES_PER_FIB), fib, DAB_FIC_BYTES_PER_FIB);
        }

        test_fic_encode_block(block, fic, block_index, 1);
    }

    for (fib_index = 0u; fib_index < 512u; ++fib_index) {
        fic[fib_index] = 0;
    }

    TEST_ASSERT(dab_fic_process_frame(&state, 77u, fic, DAB_TFRAME_FIC_SOFTBITS) == 0);
    TEST_ASSERT(state.frames_seen == 1u);
    TEST_ASSERT(state.fic_blocks_decoded_last_frame == DAB_FIC_BLOCKS_PER_FRAME);
    TEST_ASSERT(state.valid_fibs_last_frame < DAB_FIC_TOTAL_FIBS_PER_FRAME);
    dab_fic_destroy(&state);
}

void test_fic_reset_reuses_decoder_workspace(void) {
    dab_fic_state_t state;
    dab_fic_decoder_workspace_t *workspace;

    TEST_ASSERT(dab_fic_init(&state) == 0);
    workspace = state.decoder_workspace;
    TEST_ASSERT(workspace != 0);
    state.frames_seen = 7u;

    dab_fic_reset(&state);

    TEST_ASSERT(state.decoder_workspace == workspace);
    TEST_ASSERT(state.frames_seen == 0u);
    dab_fic_destroy(&state);
    TEST_ASSERT(state.decoder_workspace == 0);
}

void test_fic_cursor_iterates_figs_and_stops_on_padding(void) {
    uint8_t fib_payload[30] = {0};
    dab_fic_cursor_t cursor;
    dab_fic_fig_t fig;

    fib_payload[0] = 0x03u;
    fib_payload[1] = 0x00u;
    fib_payload[2] = 0x12u;
    fib_payload[3] = 0x34u;
    fib_payload[4] = 0xFFu;

    dab_fic_cursor_init(&cursor, fib_payload, sizeof(fib_payload));
    TEST_ASSERT(dab_fic_cursor_next_fig(&cursor, &fig) == 1);
    TEST_ASSERT(fig.type == 0u);
    TEST_ASSERT(fig.length == 3u);
    TEST_ASSERT(fig.payload[0] == 0x00u);
    TEST_ASSERT(dab_fic_cursor_next_fig(&cursor, &fig) == 0);
}

void test_fic_parse_payload_populates_repo(void) {
    dab_fic_repo_t repo = {0};
    uint8_t fig0_ext0[] = {0x03u, 0x00u, 0x12u, 0x34u};
    uint8_t fig0_ext1[5] = {0x04u, 0x01u, 0x00u, 0x00u, 0x00u};
    uint8_t fig0_ext2[] = {0x06u, 0x02u, 0x12u, 0x34u, 0x21u, 0x05u, 0x0Eu};
    uint8_t fig1_ext1[] = {
        0x35u, 0x01u, 0x12u, 0x34u,
        'T', 'E', 'S', 'T', ' ', 'S', 'E', 'R',
        'V', 'I', 'C', 'E', ' ', ' ', ' ', ' ',
        0xF0u, 0x00u
    };
    uint8_t payload0[30] = {0};
    uint8_t payload1[30] = {0};
    uint32_t bit_pos = 16u;

    test_fic_write_bits(fig0_ext1, &bit_pos, 3u, 6u);
    test_fic_write_bits(fig0_ext1, &bit_pos, 100u, 10u);
    test_fic_write_bits(fig0_ext1, &bit_pos, 0u, 1u);
    test_fic_write_bits(fig0_ext1, &bit_pos, 1u, 1u);
    test_fic_write_bits(fig0_ext1, &bit_pos, 17u, 6u);

    memcpy(payload0, fig0_ext0, sizeof(fig0_ext0));
    memcpy(payload0 + sizeof(fig0_ext0), fig0_ext1, sizeof(fig0_ext1));
    payload0[sizeof(fig0_ext0) + sizeof(fig0_ext1)] = 0xFFu;

    memcpy(payload1, fig0_ext2, sizeof(fig0_ext2));
    memcpy(payload1 + sizeof(fig0_ext2), fig1_ext1, sizeof(fig1_ext1));
    payload1[sizeof(fig0_ext2) + sizeof(fig1_ext1)] = 0xFFu;

    TEST_ASSERT(dab_fic_parse_fib_payload(&repo, payload0, sizeof(payload0)) == 0);
    TEST_ASSERT(dab_fic_parse_fib_payload(&repo, payload1, sizeof(payload1)) == 0);
    TEST_ASSERT(repo.current.eid == 0x1234u);
    TEST_ASSERT(repo.current.subchannels[3].valid == 1u);
    TEST_ASSERT(repo.current.subchannels[3].start_cu == 100u);
    TEST_ASSERT(repo.current.subchannels[3].protection_kind == DAB_FIC_PROTECTION_UEP);
    TEST_ASSERT(repo.current.subchannels[3].table_switch == 1u);
    TEST_ASSERT(repo.current.subchannels[3].protection_index == 17u);
    TEST_ASSERT(repo.current.service_count == 1u);
    TEST_ASSERT(repo.current.services[0].sid == 0x1234u);
    TEST_ASSERT(repo.current.services[0].is_programme == 1u);
    TEST_ASSERT(repo.current.services[0].caid == 2u);
    TEST_ASSERT(repo.current.services[0].component_count == 1u);
    TEST_ASSERT(repo.current.services[0].components[0].tmid == 0u);
    TEST_ASSERT(repo.current.services[0].components[0].sc_type == 5u);
    TEST_ASSERT(repo.current.services[0].components[0].subchid == 3u);
    TEST_ASSERT(repo.current.services[0].components[0].primary == 1u);
    TEST_ASSERT(repo.current.label_count == 1u);
    TEST_ASSERT(strcmp(repo.current.labels[0].text, "TEST SERVICE") == 0);
}

void test_fic_parse_payload_populates_packet_component_from_fig0_ext3(void) {
    dab_fic_repo_t repo = {0};
    uint8_t fig0_ext2[] = {0x06u, 0x02u, 0x12u, 0x34u, 0x21u, 0xC1u, 0x23u};
    uint8_t fig0_ext3[] = {
        0x06u, 0x03u,
        0x12u, 0x30u,
        0x85u,
        0x04u,
        0x12u
    };
    uint8_t payload[30] = {0};

    memcpy(payload, fig0_ext2, sizeof(fig0_ext2));
    memcpy(payload + sizeof(fig0_ext2), fig0_ext3, sizeof(fig0_ext3));
    payload[sizeof(fig0_ext2) + sizeof(fig0_ext3)] = 0xFFu;

    TEST_ASSERT(dab_fic_parse_fib_payload(&repo, payload, sizeof(payload)) == 0);
    TEST_ASSERT(repo.current.service_count == 1u);
    TEST_ASSERT(repo.current.services[0].component_count == 1u);
    TEST_ASSERT(repo.current.services[0].components[0].tmid == 3u);
    TEST_ASSERT(repo.current.services[0].components[0].scid == 0x123u);
    TEST_ASSERT(repo.current.services[0].components[0].sc_type == 5u);
    TEST_ASSERT(repo.current.services[0].components[0].subchid == 1u);
    TEST_ASSERT(repo.current.services[0].components[0].has_subchid == 1u);
    TEST_ASSERT(repo.current.services[0].components[0].packet_addr == 0x012u);
    TEST_ASSERT(repo.current.services[0].components[0].has_packet_addr == 1u);
    TEST_ASSERT(repo.current.services[0].components[0].dg_flag == 1u);
    TEST_ASSERT(repo.current.services[0].components[0].has_caorg == 0u);
}

void test_fic_debug_decode_block_uses_runtime_canonical_shape(void) {
    int8_t fic[DAB_TFRAME_FIC_SOFTBITS] = {0};
    uint8_t block[DAB_FIC_BYTES_PER_BLOCK];
    dab_fic_debug_block_dump_t dump;
    uint32_t fib_index;

    for (fib_index = 0u; fib_index < DAB_FIC_FIBS_PER_BLOCK; ++fib_index) {
        uint8_t fib[DAB_FIC_BYTES_PER_FIB];
        test_fic_build_valid_fib(fib, (uint8_t)(64u + fib_index));
        memcpy(block + (fib_index * DAB_FIC_BYTES_PER_FIB), fib, DAB_FIC_BYTES_PER_FIB);
    }

    test_fic_encode_block(block, fic, 0u, 1);
    TEST_ASSERT(dab_fic_debug_decode_block(fic, DAB_TFRAME_FIC_SOFTBITS, 0u, &dump) == 0);
    TEST_ASSERT(dump.info.block_index == 0u);
    TEST_ASSERT(dump.info.polarity_variant == 0u);
    TEST_ASSERT(dump.info.pack_variant == 0u);
    TEST_ASSERT(dump.info.scramble_variant == 1u);
    TEST_ASSERT(dump.info.byte_variant == 0u);
    TEST_ASSERT(dump.info.valid_fibs == DAB_FIC_FIBS_PER_BLOCK);
    TEST_ASSERT(memcmp(dump.decoded_block, block, DAB_FIC_BYTES_PER_BLOCK) == 0);
    TEST_ASSERT(memcmp(dump.packed_lsb, dump.decoded_block, DAB_FIC_BYTES_PER_BLOCK) != 0);
}

void test_fic_debug_decode_block_recovers_inverted_softbits(void) {
    int8_t fic[DAB_TFRAME_FIC_SOFTBITS] = {0};
    uint8_t block[DAB_FIC_BYTES_PER_BLOCK];
    dab_fic_debug_block_dump_t dump;
    uint32_t fib_index;

    for (fib_index = 0u; fib_index < DAB_FIC_FIBS_PER_BLOCK; ++fib_index) {
        uint8_t fib[DAB_FIC_BYTES_PER_FIB];
        test_fic_build_valid_fib(fib, (uint8_t)(96u + fib_index));
        memcpy(block + (fib_index * DAB_FIC_BYTES_PER_FIB), fib, DAB_FIC_BYTES_PER_FIB);
    }

    test_fic_encode_block(block, fic, 0u, 1);
    for (fib_index = 0u; fib_index < DAB_TFRAME_FIC_SOFTBITS; ++fib_index) {
        fic[fib_index] = (int8_t)-fic[fib_index];
    }

    TEST_ASSERT(dab_fic_debug_decode_block(fic, DAB_TFRAME_FIC_SOFTBITS, 0u, &dump) == 0);
    TEST_ASSERT(dump.info.block_index == 0u);
    TEST_ASSERT(dump.info.polarity_variant == 0u);
    TEST_ASSERT(dump.info.pack_variant == 0u);
    TEST_ASSERT(dump.info.scramble_variant == 1u);
    TEST_ASSERT(dump.info.byte_variant == 0u);
    TEST_ASSERT(dump.info.valid_fibs == 0u);
    TEST_ASSERT(memcmp(dump.decoded_block, block, DAB_FIC_BYTES_PER_BLOCK) != 0);
}

void test_fic_debug_decode_block_recovers_direct_packed_softbits(void) {
    int8_t fic[DAB_TFRAME_FIC_SOFTBITS] = {0};
    uint8_t block[DAB_FIC_BYTES_PER_BLOCK];
    dab_fic_debug_block_dump_t dump;
    uint32_t fib_index;

    for (fib_index = 0u; fib_index < DAB_FIC_FIBS_PER_BLOCK; ++fib_index) {
        uint8_t fib[DAB_FIC_BYTES_PER_FIB];
        test_fic_build_valid_fib(fib, (uint8_t)(112u + fib_index));
        memcpy(block + (fib_index * DAB_FIC_BYTES_PER_FIB), fib, DAB_FIC_BYTES_PER_FIB);
    }

    test_fic_encode_block(block, fic, 0u, 1);
    TEST_ASSERT(dab_fic_debug_decode_block(fic, DAB_TFRAME_FIC_SOFTBITS, 0u, &dump) == 0);
    TEST_ASSERT(dump.info.block_index == 0u);
    TEST_ASSERT(dump.info.polarity_variant == 0u);
    TEST_ASSERT(dump.info.pack_variant == 0u);
    TEST_ASSERT(dump.info.scramble_variant == 1u);
    TEST_ASSERT(dump.info.byte_variant == 0u);
    TEST_ASSERT(dump.info.valid_fibs == DAB_FIC_FIBS_PER_BLOCK);
    TEST_ASSERT(memcmp(dump.decoded_block, block, DAB_FIC_BYTES_PER_BLOCK) == 0);
}

void test_fic_debug_analyze_frame_finds_inverted_and_reversed_shape(void) {
    int8_t fic[DAB_TFRAME_FIC_SOFTBITS] = {0};
    uint8_t block[DAB_FIC_BYTES_PER_BLOCK];
    dab_fic_debug_report_t report;
    uint32_t block_index;
    uint32_t fib_index;

    memset(block, 0, sizeof(block));
    for (fib_index = 0u; fib_index < DAB_FIC_FIBS_PER_BLOCK; ++fib_index) {
        uint8_t fib[DAB_FIC_BYTES_PER_FIB];
        test_fic_build_valid_fib(fib, (uint8_t)(128u + fib_index));
        memcpy(block + (fib_index * DAB_FIC_BYTES_PER_FIB), fib, DAB_FIC_BYTES_PER_FIB);
    }
    for (block_index = 0u; block_index < DAB_FIC_BLOCKS_PER_FRAME; ++block_index) {
        test_fic_encode_block(block, fic, block_index, 0);
    }
    for (fib_index = 0u; fib_index < DAB_TFRAME_FIC_SOFTBITS; ++fib_index) {
        fic[fib_index] = (int8_t)-fic[fib_index];
    }

    TEST_ASSERT(dab_fic_debug_analyze_frame(fic, DAB_TFRAME_FIC_SOFTBITS, &report) == 0);
    TEST_ASSERT(report.total_valid_fibs == DAB_FIC_TOTAL_FIBS_PER_FRAME);
    for (block_index = 0u; block_index < DAB_FIC_BLOCKS_PER_FRAME; ++block_index) {
        TEST_ASSERT(report.blocks[block_index].polarity_variant == 1u);
        TEST_ASSERT(report.blocks[block_index].pack_variant == 1u);
        TEST_ASSERT(report.blocks[block_index].scramble_variant == 1u);
        TEST_ASSERT(report.blocks[block_index].byte_variant == 0u);
        TEST_ASSERT(report.blocks[block_index].valid_fibs == DAB_FIC_FIBS_PER_BLOCK);
    }
}

void test_fic_process_frame_populates_repo_from_valid_fibs(void) {
    dab_fic_state_t state;
    int8_t fic[DAB_TFRAME_FIC_SOFTBITS] = {0};
    uint8_t payload0[30] = {0};
    uint8_t payload1[30] = {0};
    uint8_t fig0_ext0[] = {0x03u, 0x00u, 0x12u, 0x34u};
    uint8_t fig0_ext1[5] = {0x04u, 0x01u, 0x00u, 0x00u, 0x00u};
    uint8_t fig0_ext2[] = {0x06u, 0x02u, 0x12u, 0x34u, 0x21u, 0x05u, 0x0Eu};
    uint8_t fig1_ext1[] = {
        0x35u, 0x01u, 0x12u, 0x34u,
        'L', 'I', 'V', 'E', ' ', 'N', 'A', 'M',
        'E', ' ', ' ', ' ', ' ', ' ', ' ', ' ',
        0xC0u, 0x00u
    };
    uint8_t fib[DAB_FIC_BYTES_PER_FIB];
    uint8_t block[DAB_FIC_BYTES_PER_BLOCK];
    uint32_t bit_pos = 16u;
    uint32_t block_index;

    TEST_ASSERT(dab_fic_init(&state) == 0);
    test_fic_write_bits(fig0_ext1, &bit_pos, 3u, 6u);
    test_fic_write_bits(fig0_ext1, &bit_pos, 100u, 10u);
    test_fic_write_bits(fig0_ext1, &bit_pos, 0u, 1u);
    test_fic_write_bits(fig0_ext1, &bit_pos, 1u, 1u);
    test_fic_write_bits(fig0_ext1, &bit_pos, 17u, 6u);

    memcpy(payload0, fig0_ext0, sizeof(fig0_ext0));
    memcpy(payload0 + sizeof(fig0_ext0), fig0_ext1, sizeof(fig0_ext1));
    payload0[sizeof(fig0_ext0) + sizeof(fig0_ext1)] = 0xFFu;

    memcpy(payload1, fig0_ext2, sizeof(fig0_ext2));
    memcpy(payload1 + sizeof(fig0_ext2), fig1_ext1, sizeof(fig1_ext1));
    payload1[sizeof(fig0_ext2) + sizeof(fig1_ext1)] = 0xFFu;

    memset(block, 0, sizeof(block));
    test_fic_build_fib_with_payload(fib, payload0, sizeof(payload0));
    memcpy(block, fib, DAB_FIC_BYTES_PER_FIB);
    test_fic_build_fib_with_payload(fib, payload1, sizeof(payload1));
    memcpy(block + DAB_FIC_BYTES_PER_FIB, fib, DAB_FIC_BYTES_PER_FIB);
    test_fic_build_fib_with_payload(fib, 0, 0u);
    memcpy(block + (2u * DAB_FIC_BYTES_PER_FIB), fib, DAB_FIC_BYTES_PER_FIB);
    test_fic_encode_block(block, fic, 0u, 1);

    for (block_index = 1u; block_index < DAB_FIC_BLOCKS_PER_FRAME; ++block_index) {
        test_fic_build_fib_with_payload(fib, 0, 0u);
        memcpy(block, fib, DAB_FIC_BYTES_PER_FIB);
        memcpy(block + DAB_FIC_BYTES_PER_FIB, fib, DAB_FIC_BYTES_PER_FIB);
        memcpy(block + (2u * DAB_FIC_BYTES_PER_FIB), fib, DAB_FIC_BYTES_PER_FIB);
        test_fic_encode_block(block, fic, block_index, 1);
    }

    TEST_ASSERT(dab_fic_process_frame(&state, 100u, fic, DAB_TFRAME_FIC_SOFTBITS) == 0);
    TEST_ASSERT(state.repo.current.eid == 0x1234u);
    TEST_ASSERT(state.repo.current.subchannels[3].valid == 1u);
    TEST_ASSERT(state.repo.current.service_count == 1u);
    TEST_ASSERT(state.repo.current.labels[0].used == 1u);
    TEST_ASSERT(strcmp(state.repo.current.labels[0].text, "LIVE NAME") == 0);
    dab_fic_destroy(&state);
}
