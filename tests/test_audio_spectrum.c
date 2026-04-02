#include <math.h>

#include "dab/core/audio_spectrum.h"

#include "test_support.h"

static const float kPi = 3.14159265358979323846f;

void test_audio_spectrum_detects_tone_energy(void) {
    dab_audio_spectrum_analyzer_t analyzer;
    int16_t pcm[DAB_AUDIO_SPECTRUM_FFT_SIZE];
    const float *display = 0;
    uint32_t bin_count = 0u;
    float max_value = 0.0f;
    uint32_t i;

    dab_audio_spectrum_init(&analyzer, 48000u, 1u);

    for (i = 0u; i < DAB_AUDIO_SPECTRUM_FFT_SIZE; ++i) {
        const float sample = sinf((2.0f * kPi * 8.0f * (float)i) /
                                  (float)DAB_AUDIO_SPECTRUM_FFT_SIZE);
        pcm[i] = (int16_t)(sample * 12000.0f);
    }

    TEST_ASSERT(dab_audio_spectrum_push_i16(&analyzer,
                                            pcm,
                                            DAB_AUDIO_SPECTRUM_FFT_SIZE,
                                            1u,
                                            &display,
                                            &bin_count) != 0);
    TEST_ASSERT(display != 0);
    TEST_ASSERT(bin_count == DAB_AUDIO_SPECTRUM_DISPLAY_BINS);

    for (i = 0u; i < bin_count; ++i) {
        if (display[i] > max_value) {
            max_value = display[i];
        }
    }

    TEST_ASSERT(max_value > 0.4f);
}
