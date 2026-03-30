#include "dab/core/fic.h"

#include <limits.h>
#include <stddef.h>
#include <string.h>

#include "viterbi47_port.h"

#define DAB_FIC_INFO_BITS 768u
#define DAB_FIC_TAIL_BITS 6u
#define DAB_FIC_VITERBI_STATES 64u
#define DAB_FIC_NEG_INF (INT_MIN / 4)

static uint32_t dab_fic_puncturing_code(uint32_t p) {
    switch (p) {
        case 15u: return 0x37777777u;
        case 16u: return 0x77777777u;
        case 25u: return 0x00333333u;
        default:  return 0u;
    }
}

static uint32_t dab_fic_get_puncturing_code(uint32_t bit_addr) {
    uint32_t low = 0u;

    if (bit_addr >= low && bit_addr < low + (21u * 128u)) {
        return dab_fic_puncturing_code(16u);
    }
    low += 21u * 128u;
    if (bit_addr >= low && bit_addr < low + (3u * 128u)) {
        return dab_fic_puncturing_code(15u);
    }
    low += 3u * 128u;
    if (bit_addr >= low && bit_addr < low + 24u) {
        return dab_fic_puncturing_code(25u);
    }
    return 0u;
}

static void dab_fic_departition_block(const int8_t *frame_softbits,
                                      uint32_t block_index,
                                      int16_t *out_softbits) {
    uint32_t i;
    const uint32_t base = block_index * DAB_FIC_DEPARTITIONED_SOFTBITS;

    for (i = 0u; i < DAB_FIC_DEPARTITIONED_SOFTBITS; ++i) {
        out_softbits[i] = (int16_t)frame_softbits[base + i];
    }
}

static int dab_fic_depuncture_block(const int16_t *punctured,
                                    int16_t *unpunctured) {
    uint32_t in_pos = 0u;
    uint32_t out_pos = 0u;

    if (punctured == 0 || unpunctured == 0) {
        return -1;
    }

    while (out_pos < DAB_FIC_UNPUNCTURED_BITS) {
        const uint32_t pattern = dab_fic_get_puncturing_code(out_pos);
        const uint32_t chunk_bits = (DAB_FIC_UNPUNCTURED_BITS - out_pos >= 32u)
                                        ? 32u
                                        : (DAB_FIC_UNPUNCTURED_BITS - out_pos);
        uint32_t i;

        if (pattern == 0u) {
            return -1;
        }

        for (i = 0u; i < chunk_bits; ++i) {
            if (((pattern >> i) & 1u) != 0u) {
                if (in_pos >= DAB_FIC_DEPARTITIONED_SOFTBITS) {
                    return -1;
                }
                unpunctured[out_pos + i] = punctured[in_pos++];
            } else {
                unpunctured[out_pos + i] = 0;
            }
        }
        out_pos += chunk_bits;
    }

    return in_pos == DAB_FIC_DEPARTITIONED_SOFTBITS ? 0 : -1;
}

static int dab_fic_viterbi_decode(const int16_t *symbols, uint8_t *bits_out) {
    unsigned char metrics[DAB_FIC_UNPUNCTURED_BITS];
    void *vp;
    uint32_t i;

    if (symbols == 0 || bits_out == 0) {
        return -1;
    }

    for (i = 0u; i < DAB_FIC_UNPUNCTURED_BITS; ++i) {
        int value = (int)symbols[i] + 127;
        if (value < 0) {
            value = 0;
        } else if (value > 255) {
            value = 255;
        }
        metrics[i] = (unsigned char)value;
    }

    vp = create_viterbi47_port((int)DAB_FIC_INFO_BITS);
    if (vp == 0) {
        return -1;
    }
    if (init_viterbi47_port(vp, 0) != 0) {
        delete_viterbi47_port(vp);
        return -1;
    }
    if (update_viterbi47_blk_port(vp, metrics, (int)DAB_FIC_TRELLIS_BITS) != 0) {
        delete_viterbi47_port(vp);
        return -1;
    }
    memset(bits_out, 0, DAB_FIC_BYTES_PER_BLOCK);
    if (chainback_viterbi47_port(vp, bits_out, DAB_FIC_INFO_BITS, 0) != 0) {
        delete_viterbi47_port(vp);
        return -1;
    }
    delete_viterbi47_port(vp);

    return 0;
}

static uint8_t dab_fic_prbs_bit(int reset) {
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

static uint8_t dab_fic_prbs8(uint32_t reset) {
    uint8_t value = 0u;
    uint32_t i;

    for (i = 0u; i < 8u; ++i) {
        value = (uint8_t)((value << 1u) | dab_fic_prbs_bit(reset != 0u && i == 0u));
    }

    return value;
}

static uint8_t dab_fic_bit_swap_8(uint8_t value) {
    value = (uint8_t)(((value & 0x55u) << 1u) | ((value & 0xAAu) >> 1u));
    value = (uint8_t)(((value & 0x33u) << 2u) | ((value & 0xCCu) >> 2u));
    value = (uint8_t)(((value & 0x0Fu) << 4u) | ((value & 0xF0u) >> 4u));
    return value;
}

static void dab_fic_unpack_viterbi_bytes(const uint8_t *packed,
                                         uint32_t pack_variant,
                                         uint8_t *scrambled) {
    uint32_t i;

    for (i = 0u; i < DAB_FIC_BYTES_PER_BLOCK; ++i) {
        scrambled[i] = pack_variant == 0u ? packed[i] : dab_fic_bit_swap_8(packed[i]);
    }
}

static void dab_fic_descramble_block(const uint8_t *scrambled,
                                     uint8_t *decoded_block) {
    uint32_t i;

    for (i = 0u; i < DAB_FIC_BYTES_PER_BLOCK; ++i) {
        decoded_block[i] = (uint8_t)(scrambled[i] ^ dab_fic_prbs8(i == 0u));
    }
}

static uint16_t dab_fic_crc16_ccitt(const uint8_t *buf, size_t len) {
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

static int dab_fic_check_fib_crc(const uint8_t fib[DAB_FIC_BYTES_PER_FIB]) {
    const uint16_t calc = dab_fic_crc16_ccitt(fib, DAB_FIC_BYTES_PER_FIB - 2u);
    const uint16_t rx = (uint16_t)(((uint16_t)fib[30] << 8u) | fib[31]);
    return calc == rx;
}

static uint32_t dab_fic_count_valid_fibs(const uint8_t decoded_block[DAB_FIC_BYTES_PER_BLOCK]) {
    uint32_t valid = 0u;
    uint32_t fib_index;

    for (fib_index = 0u; fib_index < DAB_FIC_FIBS_PER_BLOCK; ++fib_index) {
        if (dab_fic_check_fib_crc(decoded_block + (fib_index * DAB_FIC_BYTES_PER_FIB)) != 0) {
            valid++;
        }
    }

    return valid;
}

static int dab_fic_decode_block_variant(const int16_t *unpunctured,
                                        uint32_t polarity_variant,
                                        uint32_t pack_variant,
                                        uint8_t decoded_block[DAB_FIC_BYTES_PER_BLOCK]) {
    int16_t trial_symbols[DAB_FIC_UNPUNCTURED_BITS];
    uint8_t packed_block[DAB_FIC_BYTES_PER_BLOCK];
    uint8_t scrambled_block[DAB_FIC_BYTES_PER_BLOCK];
    uint32_t i;

    if (unpunctured == 0 || decoded_block == 0) {
        return -1;
    }

    for (i = 0u; i < DAB_FIC_UNPUNCTURED_BITS; ++i) {
        trial_symbols[i] = polarity_variant == 0u ? unpunctured[i] : (int16_t)-unpunctured[i];
    }

    if (dab_fic_viterbi_decode(trial_symbols, packed_block) != 0) {
        return -1;
    }
    dab_fic_unpack_viterbi_bytes(packed_block, pack_variant, scrambled_block);
    dab_fic_descramble_block(scrambled_block, decoded_block);
    return 0;
}

static int dab_fic_decode_block(const int8_t *frame_softbits,
                                uint32_t block_index,
                                uint8_t decoded_block[DAB_FIC_BYTES_PER_BLOCK],
                                dab_fic_debug_block_t *debug_block) {
    int16_t depunctured[DAB_FIC_DEPARTITIONED_SOFTBITS];
    int16_t unpunctured[DAB_FIC_UNPUNCTURED_BITS];

    dab_fic_departition_block(frame_softbits, block_index, depunctured);
    if (dab_fic_depuncture_block(depunctured, unpunctured) != 0) {
        return -1;
    }
    if (dab_fic_decode_block_variant(unpunctured, 0u, 0u, decoded_block) != 0) {
        return -1;
    }

    if (debug_block != 0) {
        debug_block->block_index = block_index;
        debug_block->polarity_variant = 0u;
        debug_block->pack_variant = 0u;
        debug_block->scramble_variant = 1u;
        debug_block->byte_variant = 0u;
        debug_block->valid_fibs = dab_fic_count_valid_fibs(decoded_block);
    }
    return 0;
}

int dab_fic_debug_decode_block(const int8_t *fic_softbits,
                               uint32_t fic_softbit_count,
                               uint32_t block_index,
                               dab_fic_debug_block_dump_t *dump) {
    int16_t trial_symbols[DAB_FIC_UNPUNCTURED_BITS];
    uint8_t decoded_bytes[DAB_FIC_BYTES_PER_BLOCK];
    uint32_t polarity_variant;

    if (fic_softbits == 0 || fic_softbit_count != DAB_TFRAME_FIC_SOFTBITS ||
        dump == 0 || block_index >= DAB_FIC_BLOCKS_PER_FRAME) {
        return -1;
    }

    memset(dump, 0, sizeof(*dump));
    dab_fic_departition_block(fic_softbits, block_index, dump->depunctured);
    if (dab_fic_depuncture_block(dump->depunctured, dump->unpunctured) != 0) {
        return -1;
    }

    if (dab_fic_decode_block(fic_softbits, block_index, dump->decoded_block, &dump->info) != 0) {
        return -1;
    }

    polarity_variant = dump->info.polarity_variant;
    {
        uint32_t i;
        for (i = 0u; i < DAB_FIC_UNPUNCTURED_BITS; ++i) {
            trial_symbols[i] = polarity_variant == 0u ? dump->unpunctured[i] : (int16_t)-dump->unpunctured[i];
        }
    }
    if (dab_fic_viterbi_decode(trial_symbols, decoded_bytes) != 0) {
        return -1;
    }
    memset(dump->decoded_bits, 0, sizeof(dump->decoded_bits));
    memcpy(dump->packed_lsb, decoded_bytes, sizeof(dump->packed_lsb));
    memcpy(dump->packed_msb, decoded_bytes, sizeof(dump->packed_msb));

    return 0;
}

int dab_fic_init(dab_fic_state_t *state) {
    if (state == 0) {
        return -1;
    }
    dab_fic_reset(state);
    return 0;
}

void dab_fic_reset(dab_fic_state_t *state) {
    if (state == 0) {
        return;
    }
    *state = (dab_fic_state_t){0};
}

int dab_fic_process_frame(dab_fic_state_t *state,
                          uint64_t frame_start_sample,
                          const int8_t *fic_softbits,
                          uint32_t fic_softbit_count) {
    uint32_t block_index;

    if (state == 0 || fic_softbits == 0 || fic_softbit_count != DAB_TFRAME_FIC_SOFTBITS) {
        return -1;
    }

    state->frames_seen++;
    state->last_frame_start_sample = frame_start_sample;
    state->fic_blocks_decoded_last_frame = 0u;
    state->valid_fibs_last_frame = 0u;
    memset(state->block_info_last_frame, 0, sizeof(state->block_info_last_frame));
    memset(state->decoded_fibs, 0, sizeof(state->decoded_fibs));
    memset(state->fib_crc_ok, 0, sizeof(state->fib_crc_ok));

    for (block_index = 0u; block_index < DAB_FIC_BLOCKS_PER_FRAME; ++block_index) {
        uint8_t decoded_block[DAB_FIC_BYTES_PER_BLOCK];
        dab_fic_debug_block_t *block_info = &state->block_info_last_frame[block_index];
        uint32_t fib_index;

        if (dab_fic_decode_block(fic_softbits, block_index, decoded_block, block_info) != 0) {
            continue;
        }

        state->fic_blocks_decoded_last_frame++;
        for (fib_index = 0u; fib_index < DAB_FIC_FIBS_PER_BLOCK; ++fib_index) {
            const uint32_t global_fib_index = (block_index * DAB_FIC_FIBS_PER_BLOCK) + fib_index;
            uint8_t *dst = state->decoded_fibs[global_fib_index];

            memcpy(dst,
                   decoded_block + (fib_index * DAB_FIC_BYTES_PER_FIB),
                   DAB_FIC_BYTES_PER_FIB);
            state->fib_crc_ok[global_fib_index] = (uint8_t)dab_fic_check_fib_crc(dst);
            if (state->fib_crc_ok[global_fib_index] != 0u) {
                state->valid_fibs_last_frame++;
                state->total_valid_fibs++;
            }
        }
    }

    return 0;
}

int dab_fic_debug_analyze_frame(const int8_t *fic_softbits,
                                uint32_t fic_softbit_count,
                                dab_fic_debug_report_t *report) {
    uint32_t block_index;

    if (fic_softbits == 0 || fic_softbit_count != DAB_TFRAME_FIC_SOFTBITS || report == 0) {
        return -1;
    }

    memset(report, 0, sizeof(*report));
    for (block_index = 0u; block_index < DAB_FIC_BLOCKS_PER_FRAME; ++block_index) {
        int16_t depunctured[DAB_FIC_DEPARTITIONED_SOFTBITS];
        int16_t unpunctured[DAB_FIC_UNPUNCTURED_BITS];
        uint8_t candidate[DAB_FIC_BYTES_PER_BLOCK];
        uint32_t polarity_variant;
        uint32_t best_valid = 0u;
        uint32_t best_polarity = 0u;
        uint32_t best_pack = 0u;

        dab_fic_departition_block(fic_softbits, block_index, depunctured);
        if (dab_fic_depuncture_block(depunctured, unpunctured) != 0) {
            continue;
        }

        for (polarity_variant = 0u; polarity_variant < 2u; ++polarity_variant) {
            uint32_t pack_variant;

            for (pack_variant = 0u; pack_variant < 2u; ++pack_variant) {
                const int rc = dab_fic_decode_block_variant(unpunctured,
                                                            polarity_variant,
                                                            pack_variant,
                                                            candidate);
                uint32_t valid;

                if (rc != 0) {
                    continue;
                }
                valid = dab_fic_count_valid_fibs(candidate);
                if (valid > best_valid || (polarity_variant == 0u && pack_variant == 0u)) {
                    best_valid = valid;
                    best_polarity = polarity_variant;
                    best_pack = pack_variant;
                }
            }
        }

        report->blocks[block_index].block_index = block_index;
        report->blocks[block_index].polarity_variant = best_polarity;
        report->blocks[block_index].pack_variant = best_pack;
        report->blocks[block_index].scramble_variant = 1u;
        report->blocks[block_index].byte_variant = 0u;
        report->blocks[block_index].valid_fibs = best_valid;
        report->total_valid_fibs += best_valid;
    }

    return 0;
}
