#define COBJMACROS

#include "audio_output_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <windows.h>
#include <audioclient.h>
#include <mmdeviceapi.h>

static const CLSID kDabClsidMmDeviceEnumerator = {
    0xbcde0395, 0xe52f, 0x467c, {0x8e, 0x3d, 0xc4, 0x57, 0x92, 0x91, 0x69, 0x2e}
};
static const IID kDabIidMmDeviceEnumerator = {
    0xa95664d2, 0x9614, 0x4f35, {0xa7, 0x46, 0xde, 0x8d, 0xb6, 0x36, 0x17, 0xe6}
};
static const IID kDabIidAudioClient = {
    0x1cb9ad4c, 0xdbfa, 0x4c32, {0xb1, 0x78, 0xc2, 0xf5, 0x68, 0xa7, 0x03, 0xb2}
};
static const IID kDabIidAudioRenderClient = {
    0xf294acfc, 0x3146, 0x4483, {0xa7, 0xbf, 0xad, 0xdc, 0xa7, 0xc2, 0x60, 0xe2}
};

typedef struct {
    IMMDeviceEnumerator *enumerator;
    IMMDevice *device;
    IAudioClient *client;
    IAudioRenderClient *render;
    HANDLE event_handle;
    UINT32 buffer_frames;
    uint16_t block_align;
    int com_initialized;
    int started;
    volatile LONG interrupted;
} dab_audio_output_wasapi_state_t;

static void dab_audio_output_wasapi_error(char *error,
                                          size_t error_size,
                                          const char *operation,
                                          HRESULT result) {
    snprintf(error,
             error_size,
             "%s failed with HRESULT 0x%08lx",
             operation,
             (unsigned long)result);
}

static void dab_audio_output_wasapi_close(void *opaque) {
    dab_audio_output_wasapi_state_t *state = (dab_audio_output_wasapi_state_t *)opaque;

    if (state == 0) {
        return;
    }
    if (state->client != 0 && state->started != 0) {
        (void)IAudioClient_Stop(state->client);
    }
    if (state->render != 0) {
        IAudioRenderClient_Release(state->render);
    }
    if (state->client != 0) {
        IAudioClient_Release(state->client);
    }
    if (state->device != 0) {
        IMMDevice_Release(state->device);
    }
    if (state->enumerator != 0) {
        IMMDeviceEnumerator_Release(state->enumerator);
    }
    if (state->event_handle != 0) {
        CloseHandle(state->event_handle);
    }
    if (state->com_initialized != 0) {
        CoUninitialize();
    }
    free(state);
}

static int dab_audio_output_wasapi_open(void **out_state,
                                       uint32_t sample_rate,
                                       uint8_t channels,
                                       uint32_t frames_per_buffer,
                                       char *error,
                                       size_t error_size) {
    dab_audio_output_wasapi_state_t *state;
    WAVEFORMATEX format;
    DWORD stream_flags;
    HRESULT result;

    if (out_state == 0) {
        return -1;
    }
    (void)frames_per_buffer;
    state = (dab_audio_output_wasapi_state_t *)calloc(1u, sizeof(*state));
    if (state == 0) {
        snprintf(error, error_size, "WASAPI state allocation failed");
        return -1;
    }
    result = CoInitializeEx(0, COINIT_MULTITHREADED);
    if (FAILED(result)) {
        dab_audio_output_wasapi_error(error, error_size, "CoInitializeEx", result);
        free(state);
        return -1;
    }
    state->com_initialized = 1;
    result = CoCreateInstance(&kDabClsidMmDeviceEnumerator,
                              0,
                              CLSCTX_ALL,
                              &kDabIidMmDeviceEnumerator,
                              (void **)&state->enumerator);
    if (FAILED(result)) {
        dab_audio_output_wasapi_error(error, error_size, "CoCreateInstance", result);
        dab_audio_output_wasapi_close(state);
        return -1;
    }
    result = IMMDeviceEnumerator_GetDefaultAudioEndpoint(state->enumerator,
                                                          eRender,
                                                          eMultimedia,
                                                          &state->device);
    if (FAILED(result)) {
        dab_audio_output_wasapi_error(error, error_size, "GetDefaultAudioEndpoint", result);
        dab_audio_output_wasapi_close(state);
        return -1;
    }
    result = IMMDevice_Activate(state->device,
                                &kDabIidAudioClient,
                                CLSCTX_ALL,
                                0,
                                (void **)&state->client);
    if (FAILED(result)) {
        dab_audio_output_wasapi_error(error, error_size, "IMMDevice::Activate", result);
        dab_audio_output_wasapi_close(state);
        return -1;
    }

    memset(&format, 0, sizeof(format));
    format.wFormatTag = WAVE_FORMAT_PCM;
    format.nChannels = channels;
    format.nSamplesPerSec = sample_rate;
    format.wBitsPerSample = 16u;
    format.nBlockAlign = (WORD)(channels * sizeof(int16_t));
    format.nAvgBytesPerSec = sample_rate * format.nBlockAlign;
    state->block_align = format.nBlockAlign;
    stream_flags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK |
                   AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
                   AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
    result = IAudioClient_Initialize(state->client,
                                     AUDCLNT_SHAREMODE_SHARED,
                                     stream_flags,
                                     0,
                                     0,
                                     &format,
                                     0);
    if (FAILED(result)) {
        dab_audio_output_wasapi_error(error, error_size, "IAudioClient::Initialize", result);
        dab_audio_output_wasapi_close(state);
        return -1;
    }
    state->event_handle = CreateEventW(0, FALSE, FALSE, 0);
    if (state->event_handle == 0) {
        snprintf(error, error_size, "CreateEventW failed with error %lu", GetLastError());
        dab_audio_output_wasapi_close(state);
        return -1;
    }
    result = IAudioClient_SetEventHandle(state->client, state->event_handle);
    if (FAILED(result)) {
        dab_audio_output_wasapi_error(error, error_size, "IAudioClient::SetEventHandle", result);
        dab_audio_output_wasapi_close(state);
        return -1;
    }
    result = IAudioClient_GetBufferSize(state->client, &state->buffer_frames);
    if (FAILED(result)) {
        dab_audio_output_wasapi_error(error, error_size, "IAudioClient::GetBufferSize", result);
        dab_audio_output_wasapi_close(state);
        return -1;
    }
    result = IAudioClient_GetService(state->client,
                                     &kDabIidAudioRenderClient,
                                     (void **)&state->render);
    if (FAILED(result)) {
        dab_audio_output_wasapi_error(error, error_size, "IAudioClient::GetService", result);
        dab_audio_output_wasapi_close(state);
        return -1;
    }
    *out_state = state;
    return 0;
}

static int dab_audio_output_wasapi_write(void *opaque,
                                        const int16_t *pcm,
                                        uint32_t frames,
                                        char *error,
                                        size_t error_size) {
    dab_audio_output_wasapi_state_t *state = (dab_audio_output_wasapi_state_t *)opaque;
    uint32_t written = 0u;

    if (state == 0 || state->client == 0 || state->render == 0 || pcm == 0 || frames == 0u) {
        return -1;
    }
    while (written < frames) {
        UINT32 padding = 0u;
        UINT32 available;
        UINT32 chunk;
        BYTE *buffer = 0;
        HRESULT result;
        DWORD wait_result;

        if (InterlockedCompareExchange(&state->interrupted, 0, 0) != 0) {
            snprintf(error, error_size, "WASAPI output interrupted");
            return -1;
        }
        if (state->started != 0) {
            wait_result = WaitForSingleObject(state->event_handle, 500u);

            if (wait_result == WAIT_TIMEOUT) {
                continue;
            }
            if (wait_result != WAIT_OBJECT_0) {
                snprintf(error, error_size, "WASAPI event wait failed with error %lu", GetLastError());
                return -1;
            }
            if (InterlockedCompareExchange(&state->interrupted, 0, 0) != 0) {
                snprintf(error, error_size, "WASAPI output interrupted");
                return -1;
            }
            result = IAudioClient_GetCurrentPadding(state->client, &padding);
            if (FAILED(result)) {
                dab_audio_output_wasapi_error(error,
                                              error_size,
                                              "IAudioClient::GetCurrentPadding",
                                              result);
                return -1;
            }
        }
        available = state->buffer_frames > padding ? state->buffer_frames - padding : 0u;
        if (available == 0u) {
            continue;
        }
        chunk = frames - written;
        if (chunk > available) {
            chunk = available;
        }
        result = IAudioRenderClient_GetBuffer(state->render, chunk, &buffer);
        if (FAILED(result)) {
            dab_audio_output_wasapi_error(error, error_size, "IAudioRenderClient::GetBuffer", result);
            return -1;
        }
        memcpy(buffer,
               &pcm[(size_t)written * (state->block_align / sizeof(int16_t))],
               (size_t)chunk * state->block_align);
        result = IAudioRenderClient_ReleaseBuffer(state->render, chunk, 0);
        if (FAILED(result)) {
            dab_audio_output_wasapi_error(error, error_size, "IAudioRenderClient::ReleaseBuffer", result);
            return -1;
        }
        written += chunk;
        if (state->started == 0) {
            result = IAudioClient_Start(state->client);
            if (FAILED(result)) {
                dab_audio_output_wasapi_error(error, error_size, "IAudioClient::Start", result);
                return -1;
            }
            state->started = 1;
        }
    }
    return 0;
}

static void dab_audio_output_wasapi_interrupt(void *opaque) {
    dab_audio_output_wasapi_state_t *state = (dab_audio_output_wasapi_state_t *)opaque;

    if (state != 0) {
        InterlockedExchange(&state->interrupted, 1);
        if (state->event_handle != 0) {
            SetEvent(state->event_handle);
        }
    }
}

const dab_audio_output_ops_t *dab_audio_output_wasapi_ops(void) {
    static const dab_audio_output_ops_t ops = {
        "Windows Audio (WASAPI)",
        &dab_audio_output_wasapi_open,
        &dab_audio_output_wasapi_write,
        &dab_audio_output_wasapi_interrupt,
        &dab_audio_output_wasapi_close
    };

    return &ops;
}
