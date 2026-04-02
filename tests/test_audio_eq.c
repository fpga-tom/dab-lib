#include <string.h>

#include "dab/core/audio_eq.h"

#include "test_support.h"

void test_audio_eq_zero_input_stays_zero(void) {
    dab_audio_eq_t eq;
    int16_t input[2048] = {0};
    int16_t output[2048];
    size_t i;

    dab_audio_eq_init(&eq);
    TEST_ASSERT(dab_audio_eq_configure(&eq, 48000u, 1u) == 0);
    memset(output, 1, sizeof(output));
    TEST_ASSERT(dab_audio_eq_process_i16(&eq, output, input, 2048u) == 0);

    for (i = 0u; i < 2048u; ++i) {
        TEST_ASSERT(output[i] == 0);
    }
}
