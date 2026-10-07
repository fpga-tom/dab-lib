#include "dab/core/charset.h"

#include <string.h>

#include "test_support.h"

void test_dab_text_to_utf8_decodes_ebu_latin_diacritics(void) {
    const uint8_t input[] = {'L', 'u', 'k', 0x80u, 0xdcu, ' ', 0x55u, '\r', 'x'};
    char output[32];

    TEST_ASSERT(dab_text_to_utf8(DAB_TEXT_CHARSET_EBU_LATIN,
                                 input,
                                 sizeof(input),
                                 output,
                                 sizeof(output)) == 0);
    TEST_ASSERT(strcmp(output, "Luk\xc3\xa1\xc5\xa1 \xc5\xae") == 0);
}

void test_dab_text_to_utf8_decodes_ucs2(void) {
    const uint8_t input[] = {0x00u, 'R', 0x00u, 'a', 0x00u, 0xe1u, 0x00u, '\r'};
    char output[32];

    TEST_ASSERT(dab_text_to_utf8(DAB_TEXT_CHARSET_UCS2,
                                 input,
                                 sizeof(input),
                                 output,
                                 sizeof(output)) == 0);
    TEST_ASSERT(strcmp(output, "Ra\xc3\xa1") == 0);
}

void test_dab_text_to_utf8_rejects_invalid_ucs2_length(void) {
    const uint8_t input[] = {'x'};
    char output[8];

    TEST_ASSERT(dab_text_to_utf8(DAB_TEXT_CHARSET_UCS2,
                                 input,
                                 sizeof(input),
                                 output,
                                 sizeof(output)) != 0);
}
