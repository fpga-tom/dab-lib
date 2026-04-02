#ifndef DAB_CORE_AUDIO_EQ_H
#define DAB_CORE_AUDIO_EQ_H

#include <stdint.h>

#include "dab/core/prs_fft.h"

#define DAB_AUDIO_EQ_BAND_COUNT 10u
#define DAB_AUDIO_EQ_MAX_CHANNELS 2u
#define DAB_AUDIO_EQ_FFT_SIZE 1024u
#define DAB_AUDIO_EQ_HOP_SIZE (DAB_AUDIO_EQ_FFT_SIZE / 2u)
#define DAB_AUDIO_EQ_OUTPUT_CAPACITY (DAB_AUDIO_EQ_FFT_SIZE * 4u)

typedef struct {
    uint32_t sample_rate;
    uint8_t channels;
    uint8_t configured;
    uint8_t response_dirty;
    dab_prs_fft_t fft;
    float gains_db[DAB_AUDIO_EQ_BAND_COUNT];
    float filter_re[DAB_AUDIO_EQ_FFT_SIZE];
    float filter_im[DAB_AUDIO_EQ_FFT_SIZE];
    float work_re[DAB_AUDIO_EQ_FFT_SIZE];
    float work_im[DAB_AUDIO_EQ_FFT_SIZE];
    float history[DAB_AUDIO_EQ_MAX_CHANNELS][DAB_AUDIO_EQ_HOP_SIZE];
    float pending[DAB_AUDIO_EQ_MAX_CHANNELS][DAB_AUDIO_EQ_HOP_SIZE];
    uint32_t pending_count;
    float output_ring[DAB_AUDIO_EQ_MAX_CHANNELS][DAB_AUDIO_EQ_OUTPUT_CAPACITY];
    uint32_t output_read;
    uint32_t output_write;
    uint32_t output_count;
} dab_audio_eq_t;

void dab_audio_eq_init(dab_audio_eq_t *eq);
int dab_audio_eq_configure(dab_audio_eq_t *eq, uint32_t sample_rate, uint8_t channels);
void dab_audio_eq_set_gains_db(dab_audio_eq_t *eq, const float *gains_db, uint32_t count);
int dab_audio_eq_process_i16(dab_audio_eq_t *eq, int16_t *dst, const int16_t *src, uint32_t frames);

#endif
