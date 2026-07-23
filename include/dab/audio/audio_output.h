#ifndef DAB_AUDIO_AUDIO_OUTPUT_H
#define DAB_AUDIO_AUDIO_OUTPUT_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    DAB_AUDIO_OUTPUT_AUTO = 0,
    DAB_AUDIO_OUTPUT_PORTAUDIO = 1,
    DAB_AUDIO_OUTPUT_WASAPI = 2
} dab_audio_output_backend_t;

typedef struct dab_audio_output dab_audio_output_t;

dab_audio_output_t *dab_audio_output_create(dab_audio_output_backend_t backend);
int dab_audio_output_open(dab_audio_output_t *output,
                          uint32_t sample_rate,
                          uint8_t channels,
                          uint32_t frames_per_buffer);
int dab_audio_output_write(dab_audio_output_t *output,
                           const int16_t *pcm,
                           uint32_t frames);
void dab_audio_output_interrupt(dab_audio_output_t *output);
void dab_audio_output_close(dab_audio_output_t *output);
void dab_audio_output_destroy(dab_audio_output_t *output);

dab_audio_output_backend_t dab_audio_output_active_backend(const dab_audio_output_t *output);
int dab_audio_output_backend_available(dab_audio_output_backend_t backend);
const char *dab_audio_output_backend_key(dab_audio_output_backend_t backend);
const char *dab_audio_output_backend_name(dab_audio_output_backend_t backend);
dab_audio_output_backend_t dab_audio_output_backend_from_string(const char *value);
const char *dab_audio_output_last_error(const dab_audio_output_t *output);

#ifdef __cplusplus
}
#endif

#endif
