#include "dab/core/charset.h"

#include <string.h>

static uint32_t dab_ebu_latin_codepoint(uint8_t character) {
    static const uint16_t extended[128] = {
        0x00e1u, 0x00e0u, 0x00e9u, 0x00e8u, 0x00edu, 0x00ecu, 0x00f3u, 0x00f2u,
        0x00fau, 0x00f9u, 0x00d1u, 0x00c7u, 0x015eu, 0x00dfu, 0x00a1u, 0x0178u,
        0x00e2u, 0x00e4u, 0x00eau, 0x00ebu, 0x00eeu, 0x00efu, 0x00f4u, 0x00f6u,
        0x00fbu, 0x00fcu, 0x00f1u, 0x00e7u, 0x015fu, 0x011fu, 0x0131u, 0x00ffu,
        0x0136u, 0x0145u, 0x00a9u, 0x0122u, 0x011eu, 0x011bu, 0x0148u, 0x0151u,
        0x0150u, 0x20acu, 0x00a3u, 0x0024u, 0x0100u, 0x0112u, 0x012au, 0x016au,
        0x0104u, 0x0105u, 0x0116u, 0x012fu, 0x012eu, 0x0130u, 0x0144u, 0x0171u,
        0x013bu, 0x00bfu, 0x013du, 0x00b0u, 0x0141u, 0x0143u, 0x0138u, 0x00a7u,
        0x00c1u, 0x00c0u, 0x00c9u, 0x00c8u, 0x00cdu, 0x00ccu, 0x00d3u, 0x00d2u,
        0x00dau, 0x00d9u, 0x0158u, 0x010cu, 0x0160u, 0x017du, 0x00d0u, 0x013fu,
        0x00c2u, 0x00c4u, 0x00cau, 0x00cbu, 0x00ceu, 0x00cfu, 0x00d4u, 0x00d6u,
        0x00dbu, 0x00dcu, 0x0159u, 0x010du, 0x0161u, 0x017eu, 0x0111u, 0x0140u,
        0x00c3u, 0x00c5u, 0x00c6u, 0x0152u, 0x0177u, 0x00ddu, 0x00d5u, 0x00d8u,
        0x00deu, 0x014au, 0x0154u, 0x0106u, 0x015au, 0x0179u, 0x0164u, 0x00d0u,
        0x00e3u, 0x00e5u, 0x00e6u, 0x0153u, 0x0175u, 0x00fdu, 0x00f5u, 0x00f8u,
        0x00feu, 0x014bu, 0x0155u, 0x0107u, 0x015bu, 0x017au, 0x0165u, 0x0127u
    };

    if (character < 0x80u) {
        switch (character) {
            case 0x01u: return 0x0118u;
            case 0x02u: return 0x012eu;
            case 0x03u: return 0x0172u;
            case 0x04u: return 0x0102u;
            case 0x05u: return 0x0116u;
            case 0x06u: return 0x010eu;
            case 0x07u: return 0x0218u;
            case 0x08u: return 0x021au;
            case 0x09u: return 0x010au;
            case 0x0au: return 0x0120u;
            case 0x0bu: return 0x0139u;
            case 0x0cu: return 0x017bu;
            case 0x0du: return 0x0143u;
            case 0x10u: return 0x0105u;
            case 0x11u: return 0x0119u;
            case 0x12u: return 0x012fu;
            case 0x13u: return 0x0173u;
            case 0x14u: return 0x0103u;
            case 0x15u: return 0x0117u;
            case 0x16u: return 0x010fu;
            case 0x17u: return 0x0219u;
            case 0x18u: return 0x021bu;
            case 0x19u: return 0x010bu;
            case 0x1au: return 0x0147u;
            case 0x1bu: return 0x011au;
            case 0x1cu: return 0x0121u;
            case 0x1du: return 0x013au;
            case 0x1eu: return 0x017cu;
            case 0x24u: return 0x0142u;
            case 0x55u: return 0x016eu;
            case 0x5cu: return 0x0141u;
            case 0x5eu: return 0x2015u;
            case 0x60u: return 0x0104u;
            case 0x7bu: return 0x00abu;
            case 0x7cu: return 0x016fu;
            case 0x7du: return 0x00bbu;
            case 0x7eu: return 0x013du;
            case 0x7fu: return 0xfffdu;
            default: return character;
        }
    }
    return extended[character - 0x80u];
}

static size_t dab_utf8_encode(uint32_t codepoint, char output[4]) {
    if (codepoint <= 0x7fu) {
        output[0] = (char)codepoint;
        return 1u;
    }
    if (codepoint <= 0x7ffu) {
        output[0] = (char)(0xc0u | (codepoint >> 6u));
        output[1] = (char)(0x80u | (codepoint & 0x3fu));
        return 2u;
    }
    if (codepoint <= 0xffffu) {
        output[0] = (char)(0xe0u | (codepoint >> 12u));
        output[1] = (char)(0x80u | ((codepoint >> 6u) & 0x3fu));
        output[2] = (char)(0x80u | (codepoint & 0x3fu));
        return 3u;
    }
    output[0] = (char)(0xf0u | (codepoint >> 18u));
    output[1] = (char)(0x80u | ((codepoint >> 12u) & 0x3fu));
    output[2] = (char)(0x80u | ((codepoint >> 6u) & 0x3fu));
    output[3] = (char)(0x80u | (codepoint & 0x3fu));
    return 4u;
}

static uint32_t dab_read_ucs2(const uint8_t *input) {
    return ((uint32_t)input[0] << 8u) | input[1];
}

int dab_text_to_utf8(uint8_t charset,
                     const uint8_t *input,
                     size_t input_size,
                     char *output,
                     size_t output_capacity) {
    size_t input_offset = 0u;
    size_t output_size = 0u;
    size_t last_non_space_size = 0u;

    if ((input == 0 && input_size != 0u) || output == 0 || output_capacity == 0u ||
        (charset != DAB_TEXT_CHARSET_EBU_LATIN &&
         charset != DAB_TEXT_CHARSET_UCS2) ||
        (charset == DAB_TEXT_CHARSET_UCS2 && (input_size & 1u) != 0u)) {
        return -1;
    }
    output[0] = '\0';

    while (input_offset < input_size) {
        uint32_t codepoint;
        char encoded[4];
        size_t encoded_size;
        int is_carriage_return;

        if (charset == DAB_TEXT_CHARSET_EBU_LATIN) {
            const uint8_t character = input[input_offset++];
            is_carriage_return = character == '\r';
            codepoint = dab_ebu_latin_codepoint(character);
        } else {
            codepoint = dab_read_ucs2(input + input_offset);
            input_offset += 2u;
            is_carriage_return = codepoint == '\r';
            if (codepoint >= 0xd800u && codepoint <= 0xdbffu &&
                input_offset + 1u < input_size) {
                const uint32_t low = dab_read_ucs2(input + input_offset);
                if (low >= 0xdc00u && low <= 0xdfffu) {
                    codepoint = 0x10000u + ((codepoint - 0xd800u) << 10u) +
                                (low - 0xdc00u);
                    input_offset += 2u;
                } else {
                    codepoint = 0xfffdu;
                }
            } else if (codepoint >= 0xd800u && codepoint <= 0xdfffu) {
                codepoint = 0xfffdu;
            }
        }

        if (is_carriage_return != 0) {
            break;
        }
        if (codepoint < 0x20u ||
            (charset == DAB_TEXT_CHARSET_UCS2 &&
             codepoint >= 0x7fu && codepoint <= 0x9fu)) {
            continue;
        }

        encoded_size = dab_utf8_encode(codepoint, encoded);
        if (encoded_size > output_capacity - output_size - 1u) {
            break;
        }
        memcpy(output + output_size, encoded, encoded_size);
        output_size += encoded_size;
        if (codepoint != ' ') {
            last_non_space_size = output_size;
        }
    }

    output_size = last_non_space_size;
    output[output_size] = '\0';
    return 0;
}
