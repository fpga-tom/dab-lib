#include "dab/audio/audio_output.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "audio_output_internal.h"

enum {
    DAB_AUDIO_OUTPUT_ERROR_SIZE = 256
};

struct dab_audio_output {
    dab_audio_output_backend_t requested_backend;
    dab_audio_output_backend_t active_backend;
    const dab_audio_output_ops_t *ops;
    void *state;
    char last_error[DAB_AUDIO_OUTPUT_ERROR_SIZE];
};

static const dab_audio_output_ops_t *dab_audio_output_ops(dab_audio_output_backend_t backend) {
    switch (backend) {
#if defined(DAB_AUDIO_OUTPUT_HAVE_PORTAUDIO)
        case DAB_AUDIO_OUTPUT_PORTAUDIO:
            return dab_audio_output_portaudio_ops();
#endif
#if defined(DAB_AUDIO_OUTPUT_HAVE_WASAPI)
        case DAB_AUDIO_OUTPUT_WASAPI:
            return dab_audio_output_wasapi_ops();
#endif
        default:
            return 0;
    }
}

static int dab_audio_output_try_open(dab_audio_output_t *output,
                                     dab_audio_output_backend_t backend,
                                     uint32_t sample_rate,
                                     uint8_t channels,
                                     uint32_t frames_per_buffer) {
    const dab_audio_output_ops_t *ops = dab_audio_output_ops(backend);
    void *state = 0;

    if (ops == 0) {
        snprintf(output->last_error,
                 sizeof(output->last_error),
                 "%s backend is not available in this build",
                 dab_audio_output_backend_name(backend));
        return -1;
    }
    output->last_error[0] = '\0';
    if (ops->open(&state,
                  sample_rate,
                  channels,
                  frames_per_buffer,
                  output->last_error,
                  sizeof(output->last_error)) != 0) {
        return -1;
    }
    output->ops = ops;
    output->state = state;
    output->active_backend = backend;
    return 0;
}

dab_audio_output_t *dab_audio_output_create(dab_audio_output_backend_t backend) {
    dab_audio_output_t *output;

    if (backend < DAB_AUDIO_OUTPUT_AUTO || backend > DAB_AUDIO_OUTPUT_WASAPI) {
        backend = DAB_AUDIO_OUTPUT_AUTO;
    }
    output = (dab_audio_output_t *)calloc(1u, sizeof(*output));
    if (output != 0) {
        output->requested_backend = backend;
        output->active_backend = DAB_AUDIO_OUTPUT_AUTO;
    }
    return output;
}

int dab_audio_output_open(dab_audio_output_t *output,
                          uint32_t sample_rate,
                          uint8_t channels,
                          uint32_t frames_per_buffer) {
    if (output == 0 || sample_rate == 0u || channels == 0u || channels > 2u ||
        frames_per_buffer == 0u) {
        return -1;
    }
    dab_audio_output_close(output);
    if (output->requested_backend != DAB_AUDIO_OUTPUT_AUTO) {
        return dab_audio_output_try_open(output,
                                         output->requested_backend,
                                         sample_rate,
                                         channels,
                                         frames_per_buffer);
    }

#if defined(_WIN32)
    if (dab_audio_output_try_open(output,
                                  DAB_AUDIO_OUTPUT_WASAPI,
                                  sample_rate,
                                  channels,
                                  frames_per_buffer) == 0) {
        return 0;
    }
#endif
    if (dab_audio_output_try_open(output,
                                  DAB_AUDIO_OUTPUT_PORTAUDIO,
                                  sample_rate,
                                  channels,
                                  frames_per_buffer) == 0) {
        return 0;
    }
    return -1;
}

int dab_audio_output_write(dab_audio_output_t *output,
                           const int16_t *pcm,
                           uint32_t frames) {
    if (output == 0 || output->ops == 0 || output->state == 0 || pcm == 0 || frames == 0u) {
        return -1;
    }
    output->last_error[0] = '\0';
    return output->ops->write(output->state,
                              pcm,
                              frames,
                              output->last_error,
                              sizeof(output->last_error));
}

void dab_audio_output_interrupt(dab_audio_output_t *output) {
    if (output != 0 && output->ops != 0 && output->state != 0 &&
        output->ops->interrupt != 0) {
        output->ops->interrupt(output->state);
    }
}

void dab_audio_output_close(dab_audio_output_t *output) {
    if (output == 0) {
        return;
    }
    if (output->ops != 0 && output->state != 0) {
        output->ops->close(output->state);
    }
    output->ops = 0;
    output->state = 0;
    output->active_backend = DAB_AUDIO_OUTPUT_AUTO;
}

void dab_audio_output_destroy(dab_audio_output_t *output) {
    if (output == 0) {
        return;
    }
    dab_audio_output_close(output);
    free(output);
}

dab_audio_output_backend_t dab_audio_output_active_backend(const dab_audio_output_t *output) {
    return output != 0 ? output->active_backend : DAB_AUDIO_OUTPUT_AUTO;
}

int dab_audio_output_backend_available(dab_audio_output_backend_t backend) {
    if (backend == DAB_AUDIO_OUTPUT_AUTO) {
        return dab_audio_output_ops(DAB_AUDIO_OUTPUT_WASAPI) != 0 ||
               dab_audio_output_ops(DAB_AUDIO_OUTPUT_PORTAUDIO) != 0;
    }
    return dab_audio_output_ops(backend) != 0;
}

const char *dab_audio_output_backend_key(dab_audio_output_backend_t backend) {
    switch (backend) {
        case DAB_AUDIO_OUTPUT_PORTAUDIO:
            return "portaudio";
        case DAB_AUDIO_OUTPUT_WASAPI:
            return "wasapi";
        default:
            return "auto";
    }
}

const char *dab_audio_output_backend_name(dab_audio_output_backend_t backend) {
    switch (backend) {
        case DAB_AUDIO_OUTPUT_PORTAUDIO:
            return "PortAudio";
        case DAB_AUDIO_OUTPUT_WASAPI:
            return "Windows Audio (WASAPI)";
        default:
            return "Automatic";
    }
}

dab_audio_output_backend_t dab_audio_output_backend_from_string(const char *value) {
    if (value == 0) {
        return DAB_AUDIO_OUTPUT_AUTO;
    }
    if (strcmp(value, "portaudio") == 0) {
        return DAB_AUDIO_OUTPUT_PORTAUDIO;
    }
    if (strcmp(value, "wasapi") == 0 || strcmp(value, "windows") == 0) {
        return DAB_AUDIO_OUTPUT_WASAPI;
    }
    return DAB_AUDIO_OUTPUT_AUTO;
}

const char *dab_audio_output_last_error(const dab_audio_output_t *output) {
    if (output == 0 || output->last_error[0] == '\0') {
        return "unknown audio output error";
    }
    return output->last_error;
}
