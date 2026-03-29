#include "dab/core/fic.h"

#include <string.h>

#include "test_support.h"

#define TEST_FIC_SOFTBIT_ONE 96
#define TEST_FIC_SOFTBIT_ZERO (-96)
#define TEST_FIC_TRELLIS_BITS 774u
#define TEST_FIC_INFO_BITS 768u
#define TEST_FIC_UNPUNCTURED_BITS 3096u
#define TEST_FIC_DEPARTITIONED_BITS 2304u

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
                                  uint32_t block_index) {
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
            input_bits[(byte_index * 8u) + bit_index] = (uint8_t)((scrambled[byte_index] >> bit_index) & 1u);
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

        test_fic_encode_block(block, fic, block_index);
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

        test_fic_encode_block(block, fic, block_index);
    }

    for (fib_index = 0u; fib_index < 512u; ++fib_index) {
        fic[fib_index] = 0;
    }

    TEST_ASSERT(dab_fic_process_frame(&state, 77u, fic, DAB_TFRAME_FIC_SOFTBITS) == 0);
    TEST_ASSERT(state.frames_seen == 1u);
    TEST_ASSERT(state.fic_blocks_decoded_last_frame == DAB_FIC_BLOCKS_PER_FRAME);
    TEST_ASSERT(state.valid_fibs_last_frame < DAB_FIC_TOTAL_FIBS_PER_FRAME);
}
