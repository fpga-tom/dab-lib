#include "audio_output_internal.h"

#include <stdio.h>
#include <stdlib.h>

#include <portaudio.h>

typedef struct {
    PaStream *stream;
    int initialized;
    int started;
    uint32_t underflow_count;
} dab_audio_output_portaudio_state_t;

static void dab_audio_output_portaudio_close(void *opaque) {
    dab_audio_output_portaudio_state_t *state = (dab_audio_output_portaudio_state_t *)opaque;

    if (state == 0) {
        return;
    }
    if (state->stream != 0) {
        if (state->started != 0 && Pa_IsStreamActive(state->stream) == 1) {
            (void)Pa_StopStream(state->stream);
        }
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
    PaDeviceIndex output_device;
    const PaDeviceInfo *device_info;
    PaStreamParameters output_params;
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
    output_device = Pa_GetDefaultOutputDevice();
    if (output_device == paNoDevice) {
        snprintf(error, error_size, "PortAudio has no default output device");
        dab_audio_output_portaudio_close(state);
        return -1;
    }
    device_info = Pa_GetDeviceInfo(output_device);
    if (device_info == 0) {
        snprintf(error, error_size, "PortAudio default output device information is unavailable");
        dab_audio_output_portaudio_close(state);
        return -1;
    }
    output_params.device = output_device;
    output_params.channelCount = (int)channels;
    output_params.sampleFormat = paInt16;
    output_params.suggestedLatency = device_info->defaultLowOutputLatency;
    output_params.hostApiSpecificStreamInfo = 0;
    pa_error = Pa_OpenStream(&state->stream,
                             0,
                             &output_params,
                             (double)sample_rate,
                             frames_per_buffer,
                             paNoFlag,
                             0,
                             0);
    if (pa_error != paNoError) {
        snprintf(error, error_size, "Pa_OpenStream failed: %s", Pa_GetErrorText(pa_error));
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
    if (state->started == 0) {
        pa_error = Pa_StartStream(state->stream);
        if (pa_error != paNoError) {
            snprintf(error, error_size, "Pa_StartStream failed: %s", Pa_GetErrorText(pa_error));
            return -1;
        }
        state->started = 1;
    }
    pa_error = Pa_WriteStream(state->stream, pcm, frames);
    if (pa_error == paNoError) {
        return 0;
    }
    if (pa_error == paOutputUnderflowed) {
        state->underflow_count++;
        fprintf(stderr,
                "PortAudio output underflow recovered (count=%u)\n",
                state->underflow_count);
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
