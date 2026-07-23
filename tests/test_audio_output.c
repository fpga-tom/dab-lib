#include <string.h>

#include "dab/audio/audio_output.h"

#include "test_support.h"

void test_audio_output_backend_metadata(void) {
    TEST_ASSERT(dab_audio_output_backend_from_string(0) == DAB_AUDIO_OUTPUT_AUTO);
    TEST_ASSERT(dab_audio_output_backend_from_string("auto") == DAB_AUDIO_OUTPUT_AUTO);
    TEST_ASSERT(dab_audio_output_backend_from_string("portaudio") == DAB_AUDIO_OUTPUT_PORTAUDIO);
    TEST_ASSERT(dab_audio_output_backend_from_string("wasapi") == DAB_AUDIO_OUTPUT_WASAPI);
    TEST_ASSERT(dab_audio_output_backend_from_string("windows") == DAB_AUDIO_OUTPUT_WASAPI);
    TEST_ASSERT(dab_audio_output_backend_from_string("invalid") == DAB_AUDIO_OUTPUT_AUTO);
    TEST_ASSERT(strcmp(dab_audio_output_backend_key(DAB_AUDIO_OUTPUT_AUTO), "auto") == 0);
    TEST_ASSERT(strcmp(dab_audio_output_backend_key(DAB_AUDIO_OUTPUT_PORTAUDIO), "portaudio") == 0);
    TEST_ASSERT(strcmp(dab_audio_output_backend_key(DAB_AUDIO_OUTPUT_WASAPI), "wasapi") == 0);
    TEST_ASSERT(strcmp(dab_audio_output_backend_name(DAB_AUDIO_OUTPUT_WASAPI),
                       "Windows Audio (WASAPI)") == 0);
}

void test_audio_output_creation_is_backend_independent(void) {
    dab_audio_output_t *output = dab_audio_output_create(DAB_AUDIO_OUTPUT_AUTO);
    int any_available;

    TEST_ASSERT(output != 0);
    TEST_ASSERT(dab_audio_output_active_backend(output) == DAB_AUDIO_OUTPUT_AUTO);
    any_available = dab_audio_output_backend_available(DAB_AUDIO_OUTPUT_PORTAUDIO) ||
                    dab_audio_output_backend_available(DAB_AUDIO_OUTPUT_WASAPI);
    TEST_ASSERT(dab_audio_output_backend_available(DAB_AUDIO_OUTPUT_AUTO) == any_available);
    dab_audio_output_destroy(output);
}
