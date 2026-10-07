#ifndef DAB_CORE_CHARSET_H
#define DAB_CORE_CHARSET_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    DAB_TEXT_CHARSET_EBU_LATIN = 0u,
    DAB_TEXT_CHARSET_UCS2 = 1u
};

int dab_text_to_utf8(uint8_t charset,
                     const uint8_t *input,
                     size_t input_size,
                     char *output,
                     size_t output_capacity);

#ifdef __cplusplus
}
#endif

#endif
