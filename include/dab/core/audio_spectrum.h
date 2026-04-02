#ifndef DAB_CORE_AUDIO_SPECTRUM_H
#define DAB_CORE_AUDIO_SPECTRUM_H

#include <stdint.h>

#define DAB_AUDIO_SPECTRUM_FFT_SIZE 1024u
#define DAB_AUDIO_SPECTRUM_HOP_SIZE 128u
#define DAB_AUDIO_SPECTRUM_DISPLAY_BINS 16u

typedef struct {
    uint32_t sample_rate;
    uint8_t channels;
    uint32_t write_index;
    uint32_t sample_count;
    uint32_t hop_accumulator;
    uint8_t ready;
    float ring[DAB_AUDIO_SPECTRUM_FFT_SIZE];
    float window[DAB_AUDIO_SPECTRUM_FFT_SIZE];
    float twiddle_re[DAB_AUDIO_SPECTRUM_FFT_SIZE / 2u];
    float twiddle_im[DAB_AUDIO_SPECTRUM_FFT_SIZE / 2u];
    uint16_t bit_reverse[DAB_AUDIO_SPECTRUM_FFT_SIZE];
    float fft_re[DAB_AUDIO_SPECTRUM_FFT_SIZE];
    float fft_im[DAB_AUDIO_SPECTRUM_FFT_SIZE];
    float power[DAB_AUDIO_SPECTRUM_FFT_SIZE / 2u];
    float smoothed[DAB_AUDIO_SPECTRUM_DISPLAY_BINS];
    float display[DAB_AUDIO_SPECTRUM_DISPLAY_BINS];
} dab_audio_spectrum_analyzer_t;

void dab_audio_spectrum_init(dab_audio_spectrum_analyzer_t *analyzer,
                             uint32_t sample_rate,
                             uint8_t channels);
void dab_audio_spectrum_reset(dab_audio_spectrum_analyzer_t *analyzer,
                              uint32_t sample_rate,
                              uint8_t channels);
int dab_audio_spectrum_push_i16(dab_audio_spectrum_analyzer_t *analyzer,
                                const int16_t *pcm,
                                uint32_t frames,
                                uint8_t channels,
                                const float **display_out,
                                uint32_t *bin_count_out);

#endif
