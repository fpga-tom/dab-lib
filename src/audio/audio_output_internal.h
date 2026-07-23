#ifndef DAB_AUDIO_OUTPUT_INTERNAL_H
#define DAB_AUDIO_OUTPUT_INTERNAL_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    const char *name;
    int (*open)(void **state,
                uint32_t sample_rate,
                uint8_t channels,
                uint32_t frames_per_buffer,
                char *error,
                size_t error_size);
    int (*write)(void *state,
                 const int16_t *pcm,
                 uint32_t frames,
                 char *error,
                 size_t error_size);
    void (*interrupt)(void *state);
    void (*close)(void *state);
} dab_audio_output_ops_t;

#if defined(DAB_AUDIO_OUTPUT_HAVE_PORTAUDIO)
const dab_audio_output_ops_t *dab_audio_output_portaudio_ops(void);
#endif

#if defined(DAB_AUDIO_OUTPUT_HAVE_WASAPI)
const dab_audio_output_ops_t *dab_audio_output_wasapi_ops(void);
#endif

#endif
