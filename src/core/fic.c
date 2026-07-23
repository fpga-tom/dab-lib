#include "dab/core/fic.h"

#include <limits.h>
#include <stdio.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "viterbi47_port.h"

#define DAB_FIC_INFO_BITS 768u
#define DAB_FIC_TAIL_BITS 6u
#define DAB_FIC_VITERBI_STATES 64u
#define DAB_FIC_NEG_INF (INT_MIN / 4)

struct dab_fic_decoder_workspace {
    unsigned char metrics[DAB_FIC_UNPUNCTURED_BITS];
    void *viterbi;
};

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

static int dab_fic_viterbi_decode_with_workspace(const int16_t *symbols,
                                                  uint32_t polarity_variant,
                                                  uint8_t *bits_out,
                                                  unsigned char *metrics,
                                                  void *viterbi) {
    uint32_t i;

    if (symbols == 0 || bits_out == 0 || metrics == 0 || viterbi == 0) {
        return -1;
    }

    for (i = 0u; i < DAB_FIC_UNPUNCTURED_BITS; ++i) {
        const int symbol = polarity_variant == 0u ? (int)symbols[i] : -(int)symbols[i];
        int value = symbol + 127;
        if (value < 0) {
            value = 0;
        } else if (value > 255) {
            value = 255;
        }
        metrics[i] = (unsigned char)value;
    }

    if (init_viterbi47_port(viterbi, 0) != 0) {
        return -1;
    }
    if (update_viterbi47_blk_port(viterbi, metrics, (int)DAB_FIC_TRELLIS_BITS) != 0) {
        return -1;
    }
    memset(bits_out, 0, DAB_FIC_BYTES_PER_BLOCK);
    if (chainback_viterbi47_port(viterbi, bits_out, DAB_FIC_INFO_BITS, 0) != 0) {
        return -1;
    }

    return 0;
}

static int dab_fic_viterbi_decode(const int16_t *symbols,
                                  uint32_t polarity_variant,
                                  uint8_t *bits_out) {
    unsigned char metrics[DAB_FIC_UNPUNCTURED_BITS];
    void *viterbi;
    int rc;

    if (symbols == 0 || bits_out == 0) {
        return -1;
    }

    viterbi = create_viterbi47_port((int)DAB_FIC_INFO_BITS);
    if (viterbi == 0) {
        return -1;
    }
    rc = dab_fic_viterbi_decode_with_workspace(symbols,
                                                polarity_variant,
                                                bits_out,
                                                metrics,
                                                viterbi);
    delete_viterbi47_port(viterbi);
    return rc;
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

static uint16_t dab_fic_rd16be(const uint8_t *buf) {
    return (uint16_t)(((uint16_t)buf[0] << 8u) | buf[1]);
}

static uint32_t dab_fic_rd32be(const uint8_t *buf) {
    return ((uint32_t)buf[0] << 24u) |
           ((uint32_t)buf[1] << 16u) |
           ((uint32_t)buf[2] << 8u) |
           (uint32_t)buf[3];
}

void dab_fic_cursor_init(dab_fic_cursor_t *cursor, const uint8_t *data, size_t size) {
    if (cursor == 0) {
        return;
    }
    cursor->data = data;
    cursor->remaining = size;
}

int dab_fic_cursor_next_fig(dab_fic_cursor_t *cursor, dab_fic_fig_t *fig) {
    uint8_t header;
    uint8_t type;
    uint8_t length;

    if (cursor == 0 || fig == 0 || cursor->data == 0 || cursor->remaining == 0u) {
        return 0;
    }

    header = cursor->data[0];
    type = (uint8_t)(header >> 5u);
    length = (uint8_t)(header & 0x1Fu);

    if (type == DAB_FIC_FIG_TYPE_7) {
        cursor->data += cursor->remaining;
        cursor->remaining = 0u;
        return 0;
    }
    if (length == 0u || (size_t)(1u + length) > cursor->remaining) {
        return -1;
    }

    fig->type = type;
    fig->length = length;
    fig->payload = cursor->data + 1u;
    cursor->data += (size_t)(1u + length);
    cursor->remaining -= (size_t)(1u + length);
    return 1;
}

int dab_fic_fig_view_from_raw(const dab_fic_fig_t *fig, dab_fic_fig_view_t *view) {
    uint8_t header;

    if (fig == 0 || view == 0 || fig->payload == 0) {
        return -1;
    }

    view->type = (dab_fic_fig_type_t)fig->type;
    view->ext = 0xFFu;
    view->cn = 0u;
    view->oe = 0u;
    view->pd = 0u;
    view->data = fig->payload;
    view->data_len = fig->length;

    if (fig->type == DAB_FIC_FIG_TYPE_0) {
        if (fig->length < 1u) {
            return -1;
        }
        header = fig->payload[0];
        view->cn = (uint8_t)((header >> 7u) & 1u);
        view->oe = (uint8_t)((header >> 6u) & 1u);
        view->pd = (uint8_t)((header >> 5u) & 1u);
        view->ext = (uint8_t)(header & 0x1Fu);
        view->data = fig->payload + 1u;
        view->data_len = (uint8_t)(fig->length - 1u);
    }

    return 0;
}

void dab_fic_bit_reader_init(dab_fic_bit_reader_t *reader, const uint8_t *data, size_t size) {
    if (reader == 0) {
        return;
    }
    reader->data = data;
    reader->bit_count = size * 8u;
    reader->bit_pos = 0u;
}

int dab_fic_bit_reader_get(dab_fic_bit_reader_t *reader, uint8_t bit_count, uint32_t *value) {
    uint32_t out = 0u;
    uint8_t i;

    if (reader == 0 || value == 0 || reader->data == 0 || bit_count == 0u ||
        reader->bit_pos + bit_count > reader->bit_count) {
        return -1;
    }

    for (i = 0u; i < bit_count; ++i) {
        const size_t byte_index = reader->bit_pos >> 3u;
        const size_t bit_index = 7u - (reader->bit_pos & 7u);
        out = (out << 1u) | ((reader->data[byte_index] >> bit_index) & 1u);
        reader->bit_pos++;
    }

    *value = out;
    return 0;
}

static dab_fic_database_t *dab_fic_repo_target(dab_fic_repo_t *repo, uint8_t cn) {
    if (repo == 0) {
        return 0;
    }
    return cn != 0u ? &repo->next : &repo->current;
}

static dab_fic_service_t *dab_fic_find_or_add_service(dab_fic_database_t *db,
                                                      uint8_t is_programme,
                                                      uint32_t sid) {
    uint16_t i;

    if (db == 0) {
        return 0;
    }
    for (i = 0u; i < db->service_count; ++i) {
        if (db->services[i].is_programme == is_programme && db->services[i].sid == sid) {
            return &db->services[i];
        }
    }
    if (db->service_count >= DAB_FIC_MAX_SERVICES) {
        return 0;
    }
    db->services[db->service_count] = (dab_fic_service_t){0};
    db->services[db->service_count].is_programme = is_programme;
    db->services[db->service_count].sid = sid;
    db->service_count++;
    return &db->services[db->service_count - 1u];
}

static dab_fic_service_component_t *dab_fic_add_component(dab_fic_service_t *service) {
    if (service == 0 || service->component_count >= DAB_FIC_MAX_COMPONENTS_PER_SERVICE) {
        return 0;
    }
    service->components[service->component_count] = (dab_fic_service_component_t){0};
    service->component_count++;
    return &service->components[service->component_count - 1u];
}

static dab_fic_service_component_t *dab_fic_find_packet_component(dab_fic_database_t *db,
                                                                  uint16_t scid) {
    uint16_t service_index;

    if (db == 0) {
        return 0;
    }
    for (service_index = 0u; service_index < db->service_count; ++service_index) {
        dab_fic_service_t *service = &db->services[service_index];
        uint8_t component_index;

        for (component_index = 0u; component_index < service->component_count; ++component_index) {
            dab_fic_service_component_t *component = &service->components[component_index];

            if (component->used != 0u && component->tmid == 3u && component->scid == scid) {
                return component;
            }
        }
    }

    return 0;
}

static void dab_fic_extract_label_text(char out[17], const uint8_t *chars) {
    int end = 16;
    int i;

    if (out == 0 || chars == 0) {
        return;
    }
    for (i = 0; i < 16; ++i) {
        out[i] = (char)chars[i];
    }
    while (end > 0 && (out[end - 1] == '\0' || out[end - 1] == ' ')) {
        end--;
    }
    out[end] = '\0';
}

static int dab_fic_store_label(dab_fic_database_t *db, const dab_fic_label_t *label) {
    uint16_t i;

    if (db == 0 || label == 0) {
        return -1;
    }

    for (i = 0u; i < db->label_count; ++i) {
        dab_fic_label_t *slot = &db->labels[i];
        if (slot->scope == label->scope &&
            slot->eid == label->eid &&
            slot->is_programme == label->is_programme &&
            slot->sid == label->sid &&
            slot->scids == label->scids) {
            *slot = *label;
            return 0;
        }
    }

    if (db->label_count >= DAB_FIC_MAX_LABELS) {
        return -1;
    }
    db->labels[db->label_count] = *label;
    db->label_count++;
    return 0;
}

static int dab_fic_parse_fig0_ext0(dab_fic_repo_t *repo, const dab_fic_fig_view_t *view) {
    dab_fic_database_t *db;

    if (repo == 0 || view == 0 || view->oe != 0u || view->data_len < 2u) {
        return -1;
    }
    db = dab_fic_repo_target(repo, view->cn);
    if (db == 0) {
        return -1;
    }
    db->eid = dab_fic_rd16be(view->data);
    return 0;
}

static int dab_fic_parse_fig0_ext1(dab_fic_repo_t *repo, const dab_fic_fig_view_t *view) {
    dab_fic_database_t *db;
    dab_fic_bit_reader_t reader;

    if (repo == 0 || view == 0 || view->oe != 0u) {
        return -1;
    }
    db = dab_fic_repo_target(repo, view->cn);
    if (db == 0) {
        return -1;
    }

    dab_fic_bit_reader_init(&reader, view->data, view->data_len);
    while (reader.bit_pos + 24u <= reader.bit_count) {
        dab_fic_subchannel_t subchannel = {0};
        uint32_t value = 0u;

        if (dab_fic_bit_reader_get(&reader, 6u, &value) != 0) {
            break;
        }
        subchannel.valid = 1u;
        subchannel.subchid = (uint8_t)value;
        if (dab_fic_bit_reader_get(&reader, 10u, &value) != 0) {
            break;
        }
        subchannel.start_cu = (uint16_t)value;
        if (dab_fic_bit_reader_get(&reader, 1u, &value) != 0) {
            break;
        }

        if (value == 0u) {
            subchannel.protection_kind = DAB_FIC_PROTECTION_UEP;
            if (dab_fic_bit_reader_get(&reader, 1u, &value) != 0) {
                break;
            }
            subchannel.table_switch = (uint8_t)value;
            if (dab_fic_bit_reader_get(&reader, 6u, &value) != 0) {
                break;
            }
            subchannel.protection_index = (uint8_t)value;
        } else {
            subchannel.protection_kind = DAB_FIC_PROTECTION_EEP;
            if (dab_fic_bit_reader_get(&reader, 3u, &value) != 0) {
                break;
            }
            subchannel.protection_option = (uint8_t)value;
            if (dab_fic_bit_reader_get(&reader, 2u, &value) != 0) {
                break;
            }
            subchannel.protection_level = (uint8_t)value;
            if (dab_fic_bit_reader_get(&reader, 10u, &value) != 0) {
                break;
            }
            subchannel.size_cu = (uint16_t)value;
        }

        if (subchannel.subchid < DAB_FIC_MAX_SUBCHANNELS) {
            db->subchannels[subchannel.subchid] = subchannel;
        }
    }

    return 0;
}

static int dab_fic_parse_fig0_ext2(dab_fic_repo_t *repo, const dab_fic_fig_view_t *view) {
    dab_fic_database_t *db;
    size_t offset = 0u;

    if (repo == 0 || view == 0 || view->oe != 0u) {
        return -1;
    }
    db = dab_fic_repo_target(repo, view->cn);
    if (db == 0) {
        return -1;
    }

    while (offset + (view->pd != 0u ? 4u : 2u) + 2u <= view->data_len) {
        dab_fic_service_t *service;
        uint32_t sid = view->pd != 0u ? dab_fic_rd32be(view->data + offset) :
                                        dab_fic_rd16be(view->data + offset);
        uint8_t header;
        uint8_t component_count;
        uint8_t caid;
        uint8_t i;

        offset += view->pd != 0u ? 4u : 2u;
        if (offset >= view->data_len) {
            break;
        }
        header = view->data[offset++];
        component_count = (uint8_t)(header & 0x0Fu);
        caid = (uint8_t)((header >> 4u) & 0x07u);

        service = dab_fic_find_or_add_service(db, (uint8_t)(view->pd == 0u), sid);
        if (service == 0) {
            return -1;
        }
        service->caid = caid;
        service->component_count = 0u;

        for (i = 0u; i < component_count; ++i) {
            uint16_t word;
            uint8_t tmid;
            dab_fic_service_component_t *component;

            if (offset + 2u > view->data_len) {
                offset = view->data_len;
                break;
            }
            word = dab_fic_rd16be(view->data + offset);
            offset += 2u;
            tmid = (uint8_t)((word >> 14u) & 0x03u);

            component = dab_fic_add_component(service);
            if (component == 0) {
                break;
            }
            component->used = 1u;
            component->tmid = tmid;
            if (tmid == 3u) {
                component->sc_type = (uint8_t)((word >> 8u) & 0x3Fu);
                component->scid = (uint16_t)(word & 0x0FFFu);
            } else {
                component->sc_type = (uint8_t)((word >> 8u) & 0x3Fu);
                component->subchid = (uint8_t)((word >> 2u) & 0x3Fu);
                component->has_subchid = 1u;
                component->primary = (uint8_t)((word >> 1u) & 1u);
                component->ca_flag = (uint8_t)(word & 1u);
            }
        }
    }

    return 0;
}

static int dab_fic_parse_fig0_ext3(dab_fic_repo_t *repo, const dab_fic_fig_view_t *view) {
    dab_fic_database_t *db;
    size_t offset = 0u;

    if (repo == 0 || view == 0 || view->oe != 0u) {
        return -1;
    }
    db = dab_fic_repo_target(repo, view->cn);
    if (db == 0) {
        return -1;
    }

    while (offset + 5u <= view->data_len) {
        dab_fic_service_component_t *component;
        const size_t base = offset;
        const uint16_t scid = (uint16_t)(((uint16_t)view->data[base] << 4u) |
                                         (((uint16_t)view->data[base + 1u] >> 4u) & 0x0Fu));
        const uint8_t has_caorg = (uint8_t)(view->data[base + 1u] & 1u);
        const uint8_t sc_type = (uint8_t)(view->data[base + 2u] & 0x3Fu);
        const uint8_t subchid = (uint8_t)((view->data[base + 3u] >> 2u) & 0x3Fu);
        const uint16_t packet_addr = (uint16_t)((((uint16_t)view->data[base + 3u] & 0x03u) << 8u) |
                                                (uint16_t)view->data[base + 4u]);

        offset += 5u;
        if (has_caorg != 0u) {
            if (offset + 2u > view->data_len) {
                break;
            }
        }

        component = dab_fic_find_packet_component(db, scid);
        if (component != 0) {
            component->sc_type = sc_type;
            component->subchid = subchid;
            component->has_subchid = 1u;
            component->packet_addr = packet_addr;
            component->has_packet_addr = 1u;
            component->dg_flag = (uint8_t)((view->data[base + 2u] >> 7u) & 1u);
            component->has_caorg = has_caorg;
            component->caorg = has_caorg != 0u ? dab_fic_rd16be(view->data + offset) : 0u;
        }

        if (has_caorg != 0u) {
            offset += 2u;
        }
    }

    return 0;
}

static int dab_fic_parse_fig1(dab_fic_repo_t *repo, const dab_fic_fig_view_t *view) {
    dab_fic_database_t *db;
    dab_fic_label_t label;
    const uint8_t *payload;
    size_t len;
    uint8_t header;
    uint8_t ext;
    size_t offset = 0u;

    if (repo == 0 || view == 0 || view->type != DAB_FIC_FIG_TYPE_1 || view->data_len < 1u) {
        return -1;
    }
    db = &repo->current;
    header = view->data[0];
    ext = (uint8_t)(header & 0x07u);
    payload = view->data + 1u;
    len = (size_t)(view->data_len - 1u);
    memset(&label, 0, sizeof(label));
    label.used = 1u;
    label.charset = (uint8_t)(header >> 4u);

    if (ext == 0u) {
        if (len < 20u) {
            return -1;
        }
        label.scope = DAB_FIC_LABEL_SCOPE_ENSEMBLE;
        label.eid = dab_fic_rd16be(payload);
        offset = 2u;
    } else if (ext == 1u) {
        if (len < 20u) {
            return -1;
        }
        label.scope = DAB_FIC_LABEL_SCOPE_SERVICE;
        label.is_programme = 1u;
        label.sid = dab_fic_rd16be(payload);
        offset = 2u;
    } else if (ext == 4u) {
        uint8_t pd_flag;

        if (len < 20u) {
            return -1;
        }
        label.scope = DAB_FIC_LABEL_SCOPE_COMPONENT;
        pd_flag = (uint8_t)((payload[0] >> 4u) & 1u);
        label.scids = (uint8_t)(payload[1] & 0x0Fu);
        offset = 2u;
        if (pd_flag != 0u) {
            if (len < offset + 4u + 18u) {
                return -1;
            }
            label.is_programme = 0u;
            label.sid = dab_fic_rd32be(payload + offset);
            offset += 4u;
        } else {
            if (len < offset + 2u + 18u) {
                return -1;
            }
            label.is_programme = 1u;
            label.sid = dab_fic_rd16be(payload + offset);
            offset += 2u;
        }
    } else if (ext == 5u) {
        if (len < 22u) {
            return -1;
        }
        label.scope = DAB_FIC_LABEL_SCOPE_SERVICE;
        label.is_programme = 0u;
        label.sid = dab_fic_rd32be(payload);
        offset = 4u;
    } else {
        return 0;
    }

    dab_fic_extract_label_text(label.text, payload + offset);
    offset += 16u;
    label.short_label_mask = dab_fic_rd16be(payload + offset);
    return dab_fic_store_label(db, &label);
}

int dab_fic_parse_fib_payload(dab_fic_repo_t *repo, const uint8_t *fib_payload, size_t payload_size) {
    dab_fic_cursor_t cursor;

    if (repo == 0 || fib_payload == 0 || payload_size == 0u) {
        return -1;
    }

    dab_fic_cursor_init(&cursor, fib_payload, payload_size);
    for (;;) {
        dab_fic_fig_t fig;
        dab_fic_fig_view_t view;
        const int next = dab_fic_cursor_next_fig(&cursor, &fig);

        if (next <= 0) {
            if (next < 0) {
                repo->total_fig_parse_errors++;
            }
            return next < 0 ? -1 : 0;
        }
        repo->total_figs++;
        if (dab_fic_fig_view_from_raw(&fig, &view) != 0) {
            repo->total_fig_parse_errors++;
            continue;
        }

        if (view.type == DAB_FIC_FIG_TYPE_0) {
            if (view.ext == 0u) {
                (void)dab_fic_parse_fig0_ext0(repo, &view);
            } else if (view.ext == 1u) {
                (void)dab_fic_parse_fig0_ext1(repo, &view);
            } else if (view.ext == 2u) {
                (void)dab_fic_parse_fig0_ext2(repo, &view);
            } else if (view.ext == 3u) {
                (void)dab_fic_parse_fig0_ext3(repo, &view);
            }
        } else if (view.type == DAB_FIC_FIG_TYPE_1) {
            (void)dab_fic_parse_fig1(repo, &view);
        }
    }
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
                                        uint8_t decoded_block[DAB_FIC_BYTES_PER_BLOCK],
                                        dab_fic_decoder_workspace_t *workspace) {
    uint8_t packed_block[DAB_FIC_BYTES_PER_BLOCK];
    uint8_t scrambled_block[DAB_FIC_BYTES_PER_BLOCK];

    if (unpunctured == 0 || decoded_block == 0) {
        return -1;
    }

    if (workspace != 0) {
        if (dab_fic_viterbi_decode_with_workspace(unpunctured,
                                                   polarity_variant,
                                                   packed_block,
                                                   workspace->metrics,
                                                   workspace->viterbi) != 0) {
            return -1;
        }
    } else if (dab_fic_viterbi_decode(unpunctured, polarity_variant, packed_block) != 0) {
        return -1;
    }
    dab_fic_unpack_viterbi_bytes(packed_block, pack_variant, scrambled_block);
    dab_fic_descramble_block(scrambled_block, decoded_block);
    return 0;
}

static int dab_fic_decode_block(const int8_t *frame_softbits,
                                uint32_t block_index,
                                uint8_t decoded_block[DAB_FIC_BYTES_PER_BLOCK],
                                dab_fic_debug_block_t *debug_block,
                                dab_fic_decoder_workspace_t *workspace) {
    int16_t depunctured[DAB_FIC_DEPARTITIONED_SOFTBITS];
    int16_t unpunctured[DAB_FIC_UNPUNCTURED_BITS];

    dab_fic_departition_block(frame_softbits, block_index, depunctured);
    if (dab_fic_depuncture_block(depunctured, unpunctured) != 0) {
        return -1;
    }
    if (dab_fic_decode_block_variant(unpunctured, 0u, 0u, decoded_block, workspace) != 0) {
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

    if (dab_fic_decode_block(fic_softbits, block_index, dump->decoded_block, &dump->info, 0) != 0) {
        return -1;
    }

    polarity_variant = dump->info.polarity_variant;
    if (dab_fic_viterbi_decode(dump->unpunctured, polarity_variant, decoded_bytes) != 0) {
        return -1;
    }
    memset(dump->decoded_bits, 0, sizeof(dump->decoded_bits));
    memcpy(dump->packed_lsb, decoded_bytes, sizeof(dump->packed_lsb));
    memcpy(dump->packed_msb, decoded_bytes, sizeof(dump->packed_msb));

    return 0;
}

int dab_fic_init(dab_fic_state_t *state) {
    dab_fic_decoder_workspace_t *workspace;

    if (state == 0) {
        return -1;
    }
    *state = (dab_fic_state_t){0};
    workspace = calloc(1u, sizeof(*workspace));
    if (workspace == 0) {
        return -1;
    }
    workspace->viterbi = create_viterbi47_port((int)DAB_FIC_INFO_BITS);
    if (workspace->viterbi == 0) {
        free(workspace);
        return -1;
    }
    state->decoder_workspace = workspace;
    return 0;
}

void dab_fic_reset(dab_fic_state_t *state) {
    dab_fic_decoder_workspace_t *workspace;

    if (state == 0) {
        return;
    }
    workspace = state->decoder_workspace;
    *state = (dab_fic_state_t){0};
    state->decoder_workspace = workspace;
}

void dab_fic_destroy(dab_fic_state_t *state) {
    if (state == 0) {
        return;
    }
    if (state->decoder_workspace != 0) {
        delete_viterbi47_port(state->decoder_workspace->viterbi);
        free(state->decoder_workspace);
    }
    *state = (dab_fic_state_t){0};
}

int dab_fic_process_frame(dab_fic_state_t *state,
                          uint64_t frame_start_sample,
                          const int8_t *fic_softbits,
                          uint32_t fic_softbit_count) {
    uint32_t block_index;

    if (state == 0 || state->decoder_workspace == 0 || fic_softbits == 0 ||
        fic_softbit_count != DAB_TFRAME_FIC_SOFTBITS) {
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

        if (dab_fic_decode_block(fic_softbits,
                                 block_index,
                                 decoded_block,
                                 block_info,
                                 state->decoder_workspace) != 0) {
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
                (void)dab_fic_parse_fib_payload(&state->repo, dst, DAB_FIC_BYTES_PER_FIB - 2u);
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
                                                            candidate,
                                                            0);
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
