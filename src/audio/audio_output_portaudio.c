#include "audio_output_internal.h"

#include <stdio.h>
#include <stdlib.h>

#include <portaudio.h>

typedef struct {
    PaStream *stream;
    int initialized;
} dab_audio_output_portaudio_state_t;

static void dab_audio_output_portaudio_close(void *opaque) {
    dab_audio_output_portaudio_state_t *state = (dab_audio_output_portaudio_state_t *)opaque;

    if (state == 0) {
        return;
    }
    if (state->stream != 0) {
        (void)Pa_StopStream(state->stream);
        (void)Pa_CloseStream(state->stream);
    }
    if (state->initialized != 0) {
        (void)Pa_Terminate();
    }
    free(state);
}

static int dab_audio_output_portaudio_open(void **out_state,
                                           uint32_t sample_rate,
                                           uint8_t channels,
                                           uint32_t frames_per_buffer,
                                           char *error,
                                           size_t error_size) {
    dab_audio_output_portaudio_state_t *state;
    PaError pa_error;

    if (out_state == 0) {
        return -1;
    }
    state = (dab_audio_output_portaudio_state_t *)calloc(1u, sizeof(*state));
    if (state == 0) {
        snprintf(error, error_size, "PortAudio state allocation failed");
        return -1;
    }
    pa_error = Pa_Initialize();
    if (pa_error != paNoError) {
        snprintf(error, error_size, "Pa_Initialize failed: %s", Pa_GetErrorText(pa_error));
        free(state);
        return -1;
    }
    state->initialized = 1;
    pa_error = Pa_OpenDefaultStream(&state->stream,
                                    0,
                                    (int)channels,
                                    paInt16,
                                    (double)sample_rate,
                                    frames_per_buffer,
                                    0,
                                    0);
    if (pa_error != paNoError) {
        snprintf(error, error_size, "Pa_OpenDefaultStream failed: %s", Pa_GetErrorText(pa_error));
        dab_audio_output_portaudio_close(state);
        return -1;
    }
    pa_error = Pa_StartStream(state->stream);
    if (pa_error != paNoError) {
        snprintf(error, error_size, "Pa_StartStream failed: %s", Pa_GetErrorText(pa_error));
        dab_audio_output_portaudio_close(state);
        return -1;
    }
    *out_state = state;
    return 0;
}

static int dab_audio_output_portaudio_write(void *opaque,
                                            const int16_t *pcm,
                                            uint32_t frames,
                                            char *error,
                                            size_t error_size) {
    dab_audio_output_portaudio_state_t *state = (dab_audio_output_portaudio_state_t *)opaque;
    PaError pa_error;

    if (state == 0 || state->stream == 0 || pcm == 0 || frames == 0u) {
        return -1;
    }
    pa_error = Pa_WriteStream(state->stream, pcm, frames);
    if (pa_error == paNoError || pa_error == paOutputUnderflowed) {
        return 0;
    }
    snprintf(error, error_size, "Pa_WriteStream failed: %s", Pa_GetErrorText(pa_error));
    return -1;
}

static void dab_audio_output_portaudio_interrupt(void *opaque) {
    dab_audio_output_portaudio_state_t *state = (dab_audio_output_portaudio_state_t *)opaque;

    if (state != 0 && state->stream != 0) {
        (void)Pa_AbortStream(state->stream);
    }
}

const dab_audio_output_ops_t *dab_audio_output_portaudio_ops(void) {
    static const dab_audio_output_ops_t ops = {
        "PortAudio",
        &dab_audio_output_portaudio_open,
        &dab_audio_output_portaudio_write,
        &dab_audio_output_portaudio_interrupt,
        &dab_audio_output_portaudio_close
    };

    return &ops;
}
