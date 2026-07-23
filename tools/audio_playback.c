#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "audio_playback.h"

#if defined(DAB_HAVE_AUDIO_PLAYBACK)
enum {
    DAB_AUDIO_FRAMES_PER_BUFFER = 2048,
    DAB_AUDIO_RING_SECONDS = 2,
    DAB_AUDIO_STARTUP_PREFILL_MS = 1500
};

static dab_mutex_t g_spectrum_callback_mutex;
static dab_audio_playback_spectrum_callback_t g_spectrum_callback = 0;
static void *g_spectrum_callback_user = 0;
static dab_mutex_t g_equalizer_mutex;
static float g_equalizer_gains[DAB_AUDIO_EQ_BAND_COUNT] = {0};
static uint64_t g_equalizer_gain_version = 1u;
static int g_audio_playback_globals_ready = 0;

static void dab_audio_playback_ensure_globals(void) {
    if (g_audio_playback_globals_ready == 0) {
        (void)dab_mutex_init(&g_spectrum_callback_mutex);
        (void)dab_mutex_init(&g_equalizer_mutex);
        g_audio_playback_globals_ready = 1;
    }
}

void dab_audio_playback_set_spectrum_callback(dab_audio_playback_spectrum_callback_t callback,
                                              void *user) {
    dab_audio_playback_ensure_globals();
    dab_mutex_lock(&g_spectrum_callback_mutex);
    g_spectrum_callback = callback;
    g_spectrum_callback_user = user;
    dab_mutex_unlock(&g_spectrum_callback_mutex);
}

void dab_audio_playback_set_equalizer_gains(const float *gains_db, uint32_t count) {
    uint32_t i;

    dab_audio_playback_ensure_globals();
    dab_mutex_lock(&g_equalizer_mutex);
    for (i = 0u; i < DAB_AUDIO_EQ_BAND_COUNT; ++i) {
        g_equalizer_gains[i] = (gains_db != 0 && i < count) ? gains_db[i] : 0.0f;
    }
    g_equalizer_gain_version++;
    dab_mutex_unlock(&g_equalizer_mutex);
}

static void dab_audio_playback_sync_equalizer(dab_audio_playback_t *audio) {
    float gains[DAB_AUDIO_EQ_BAND_COUNT];
    uint64_t version;

    if (audio == 0 || audio->sample_rate == 0u || audio->channels == 0u) {
        return;
    }

    dab_audio_playback_ensure_globals();
    dab_mutex_lock(&g_equalizer_mutex);
    memcpy(gains, g_equalizer_gains, sizeof(gains));
    version = g_equalizer_gain_version;
    dab_mutex_unlock(&g_equalizer_mutex);

    if (audio->eq.configured == 0u) {
        dab_audio_eq_init(&audio->eq);
        if (dab_audio_eq_configure(&audio->eq, audio->sample_rate, audio->channels) != 0) {
            return;
        }
        audio->eq_gain_version = 0u;
    }

    if (audio->eq_gain_version != version) {
        dab_audio_eq_set_gains_db(&audio->eq, gains, DAB_AUDIO_EQ_BAND_COUNT);
        audio->eq_gain_version = version;
    }
}

static int dab_audio_playback_ensure_eq_buffer(dab_audio_playback_t *audio, uint32_t frames) {
    if (audio == 0 || frames == 0u) {
        return -1;
    }
    if (audio->eq_pcm_frames < frames) {
        int16_t *next = (int16_t *)realloc(audio->eq_pcm,
                                           (size_t)frames * (size_t)audio->channels * sizeof(int16_t));
        if (next == 0) {
            return -1;
        }
        audio->eq_pcm = next;
        audio->eq_pcm_frames = frames;
    }
    return 0;
}

static void dab_audio_playback_publish_spectrum(dab_audio_playback_t *audio,
                                                const void *pcm,
                                                uint32_t frames) {
    dab_audio_playback_spectrum_callback_t callback;
    void *user;
    const float *display = 0;
    uint32_t bin_count = 0u;

    if (audio == 0 || pcm == 0 || frames == 0u || audio->sample_rate == 0u || audio->channels == 0u) {
        return;
    }

    if (dab_audio_spectrum_push_i16(&audio->spectrum,
                                    (const int16_t *)pcm,
                                    frames,
                                    audio->channels,
                                    &display,
                                    &bin_count) == 0) {
        return;
    }

    dab_audio_playback_ensure_globals();
    dab_mutex_lock(&g_spectrum_callback_mutex);
    callback = g_spectrum_callback;
    user = g_spectrum_callback_user;
    dab_mutex_unlock(&g_spectrum_callback_mutex);

    if (callback != 0 && display != 0 && bin_count != 0u) {
        callback(display, bin_count, audio->sample_rate, user);
    }
}

static unsigned int dab_audio_env_u32(const char *name, unsigned int fallback) {
    const char *value = getenv(name);
    char *end = 0;
    unsigned long parsed;

    if (value == 0 || value[0] == '\0') {
        return fallback;
    }
    parsed = strtoul(value, &end, 10);
    if (end == value || (end != 0 && *end != '\0')) {
        return fallback;
    }
    return (unsigned int)parsed;
}

static void dab_audio_ring_reset_locked(dab_audio_playback_t *audio) {
    audio->ring_read = 0u;
    audio->ring_write = 0u;
    audio->ring_count = 0u;
}

static int dab_audio_ring_init(dab_audio_playback_t *audio, size_t frames) {
    if (audio == 0 || frames == 0u || audio->channels == 0u) {
        return -1;
    }
    audio->ring = (int16_t *)malloc(frames * (size_t)audio->channels * sizeof(int16_t));
    if (audio->ring == 0) {
        return -1;
    }
    audio->ring_frames = frames;
    dab_audio_ring_reset_locked(audio);
    if (dab_mutex_init(&audio->ring_mutex) != 0) {
        free(audio->ring);
        audio->ring = 0;
        audio->ring_frames = 0u;
        return -1;
    }
    if (dab_cond_init(&audio->ring_cond) != 0) {
        dab_mutex_destroy(&audio->ring_mutex);
        free(audio->ring);
        audio->ring = 0;
        audio->ring_frames = 0u;
        return -1;
    }
    audio->ring_ready = 1u;
    return 0;
}

static void dab_audio_ring_deinit(dab_audio_playback_t *audio) {
    if (audio == 0) {
        return;
    }
    if (audio->ring_ready != 0u) {
        dab_cond_destroy(&audio->ring_cond);
        dab_mutex_destroy(&audio->ring_mutex);
        audio->ring_ready = 0u;
    }
    free(audio->ring);
    audio->ring = 0;
    audio->ring_frames = 0u;
    free(audio->eq_pcm);
    audio->eq_pcm = 0;
    audio->eq_pcm_frames = 0u;
    dab_audio_ring_reset_locked(audio);
}

static void dab_audio_ring_write_frames(dab_audio_playback_t *audio,
                                        const int16_t *pcm,
                                        size_t frames) {
    size_t first;
    size_t second;
    size_t free_frames;
    const size_t channels = (size_t)audio->channels;

    if (audio == 0 || audio->ring == 0 || audio->ring_ready == 0u || pcm == 0 || frames == 0u) {
        return;
    }
    if (frames >= audio->ring_frames) {
        const size_t keep = audio->ring_frames;

        pcm += (frames - keep) * channels;
        frames = keep;
    }

    dab_mutex_lock(&audio->ring_mutex);
    free_frames = audio->ring_frames - audio->ring_count;
    if (frames > free_frames) {
        const size_t drop = frames - free_frames;

        audio->ring_read = (audio->ring_read + drop) % audio->ring_frames;
        audio->ring_count -= drop;
    }

    first = frames;
    if (audio->ring_write + first > audio->ring_frames) {
        first = audio->ring_frames - audio->ring_write;
    }
    second = frames - first;
    memcpy(&audio->ring[audio->ring_write * channels],
           pcm,
           first * channels * sizeof(int16_t));
    if (second > 0u) {
        memcpy(&audio->ring[0],
               &pcm[first * channels],
               second * channels * sizeof(int16_t));
    }
    audio->ring_write = (audio->ring_write + frames) % audio->ring_frames;
    audio->ring_count += frames;
    dab_cond_signal(&audio->ring_cond);
    dab_mutex_unlock(&audio->ring_mutex);
}

static size_t dab_audio_ring_read_frames(dab_audio_playback_t *audio,
                                         int16_t *out,
                                         size_t frames) {
    size_t got;
    size_t first;
    size_t second;
    const size_t channels = (size_t)audio->channels;

    if (audio == 0 || out == 0 || frames == 0u || audio->ring == 0 || audio->ring_ready == 0u) {
        return 0u;
    }

    dab_mutex_lock(&audio->ring_mutex);
    got = audio->ring_count < frames ? audio->ring_count : frames;
    if (got > 0u) {
        first = got;
        if (audio->ring_read + first > audio->ring_frames) {
            first = audio->ring_frames - audio->ring_read;
        }
        second = got - first;
        memcpy(out,
               &audio->ring[audio->ring_read * channels],
               first * channels * sizeof(int16_t));
        if (second > 0u) {
            memcpy(&out[first * channels],
                   &audio->ring[0],
                   second * channels * sizeof(int16_t));
        }
        audio->ring_read = (audio->ring_read + got) % audio->ring_frames;
        audio->ring_count -= got;
    }
    dab_mutex_unlock(&audio->ring_mutex);
    return got;
}

static void dab_audio_playback_signal_output_failed(dab_audio_playback_t *audio) {
    dab_mutex_lock(&audio->ring_mutex);
    audio->output_failed = 1u;
    dab_cond_broadcast(&audio->ring_cond);
    dab_mutex_unlock(&audio->ring_mutex);
}

static int dab_audio_playback_thread_main(void *arg) {
    dab_audio_playback_t *audio = (dab_audio_playback_t *)arg;
    dab_thread_realtime_audio_scope_t realtime_scope = {0};
    dab_audio_output_t *output;
    int16_t *out;
    const size_t channels = (size_t)audio->channels;
    size_t prefill_frames;

    if (audio->realtime_enabled != 0u &&
        dab_thread_realtime_audio_begin(&realtime_scope) != 0) {
        fprintf(stderr, "audio playback: Windows realtime audio scheduling is unavailable\n");
    }
    out = (int16_t *)malloc(DAB_AUDIO_FRAMES_PER_BUFFER * channels * sizeof(int16_t));
    if (out == 0) {
        dab_audio_playback_signal_output_failed(audio);
        dab_thread_realtime_audio_end(&realtime_scope);
        return 0;
    }
    output = dab_audio_output_create(audio->output_backend);
    if (output == 0) {
        fprintf(stderr, "audio playback: output allocation failed\n");
        free(out);
        dab_audio_playback_signal_output_failed(audio);
        dab_thread_realtime_audio_end(&realtime_scope);
        return 0;
    }
    if (dab_audio_output_open(output,
                              audio->sample_rate,
                              audio->channels,
                              DAB_AUDIO_FRAMES_PER_BUFFER) != 0) {
        fprintf(stderr,
                "audio playback: output open failed (%s)\n",
                dab_audio_output_last_error(output));
        dab_audio_output_destroy(output);
        free(out);
        dab_audio_playback_signal_output_failed(audio);
        dab_thread_realtime_audio_end(&realtime_scope);
        return 0;
    }
    dab_mutex_lock(&audio->ring_mutex);
    audio->output = output;
    audio->output_ready = 1u;
    dab_cond_broadcast(&audio->ring_cond);
    dab_mutex_unlock(&audio->ring_mutex);
    printf("audio playback output=%s sample_rate=%u channels=%u\n",
           dab_audio_output_backend_name(dab_audio_output_active_backend(output)),
           audio->sample_rate,
           audio->channels);
    prefill_frames =
        ((size_t)audio->sample_rate * DAB_AUDIO_STARTUP_PREFILL_MS) / 1000u;
    if (prefill_frames < DAB_AUDIO_FRAMES_PER_BUFFER) {
        prefill_frames = DAB_AUDIO_FRAMES_PER_BUFFER;
    }
    if (prefill_frames > audio->ring_frames) {
        prefill_frames = audio->ring_frames;
    }

    while (audio->thread_stop == 0u) {
        if (audio->ring_ready != 0u) {
            size_t required_frames;

            dab_mutex_lock(&audio->ring_mutex);
            required_frames = audio->prebuffer_done == 0u ?
                prefill_frames : DAB_AUDIO_FRAMES_PER_BUFFER;
            while (audio->thread_stop == 0u &&
                   audio->ring_count < required_frames) {
                dab_cond_wait(&audio->ring_cond, &audio->ring_mutex);
            }
            if (audio->thread_stop == 0u && audio->ring_count >= prefill_frames) {
                audio->prebuffer_done = 1u;
            }
            dab_mutex_unlock(&audio->ring_mutex);
        }
        if (audio->thread_stop != 0u) {
            break;
        }
        {
            size_t got = dab_audio_ring_read_frames(audio, out, DAB_AUDIO_FRAMES_PER_BUFFER);

            if (got != DAB_AUDIO_FRAMES_PER_BUFFER) {
                continue;
            }
            if (dab_audio_output_write(output, out, DAB_AUDIO_FRAMES_PER_BUFFER) != 0) {
                fprintf(stderr,
                        "audio playback: output write failed (%s)\n",
                        dab_audio_output_last_error(output));
                break;
            }
        }
    }
    dab_mutex_lock(&audio->ring_mutex);
    audio->output = 0;
    audio->output_ready = 0u;
    dab_mutex_unlock(&audio->ring_mutex);
    dab_audio_output_destroy(output);
    free(out);
    dab_thread_realtime_audio_end(&realtime_scope);
    return 0;
}

static void dab_audio_playback_stop_thread(dab_audio_playback_t *audio) {
    if (audio == 0) {
        return;
    }
    if (audio->ring_ready != 0u) {
        dab_mutex_lock(&audio->ring_mutex);
        audio->thread_stop = 1u;
        if (audio->output != 0) {
            dab_audio_output_interrupt(audio->output);
        }
        dab_cond_broadcast(&audio->ring_cond);
        dab_mutex_unlock(&audio->ring_mutex);
    } else {
        audio->thread_stop = 1u;
    }
    if (audio->thread_running != 0u) {
        (void)dab_thread_join(&audio->thread, 0);
        audio->thread_running = 0u;
    }
}
static void dab_audio_playback_print_hex(const char *label,
                                         const uint8_t *data,
                                         uint32_t length,
                                         uint32_t max_bytes) {
    uint32_t i;
    uint32_t print_count;

    if (label == 0 || data == 0) {
        return;
    }
    print_count = length < max_bytes ? length : max_bytes;
    printf("%s len=%u", label, length);
    for (i = 0u; i < print_count; ++i) {
        printf(" %02x", data[i]);
    }
    if (print_count < length) {
        printf(" ...");
    }
    printf("\n");
}

static uint8_t dabplus_core_sample_rate_index(const dab_msc_dabplus_superframe_t *superframe) {
    if (superframe->dac_rate != 0u) {
        return (uint8_t)(superframe->sbr_flag != 0u ? 6u : 3u);
    }
    return (uint8_t)(superframe->sbr_flag != 0u ? 8u : 5u);
}

static uint8_t dabplus_extension_sample_rate_index(const dab_msc_dabplus_superframe_t *superframe) {
    return (uint8_t)(superframe->dac_rate != 0u ? 3u : 5u);
}

static uint8_t dabplus_channel_config(const dab_msc_dabplus_superframe_t *superframe) {
    return (uint8_t)(superframe->aac_channel_mode != 0u ? 2u : 1u);
}

static uint32_t dab_audio_playback_build_asc(const dab_msc_dabplus_superframe_t *superframe,
                                             uint8_t *asc,
                                             uint32_t asc_capacity) {
    uint32_t asc_length = 0u;
    const uint8_t core_sr = dabplus_core_sample_rate_index(superframe);

    if (superframe == 0 || asc == 0 || asc_capacity < 7u) {
        return 0u;
    }

    asc[asc_length++] = (uint8_t)((2u << 3u) | (core_sr >> 1u));
    asc[asc_length++] = (uint8_t)(((core_sr & 1u) << 7u) |
                                  (dabplus_channel_config(superframe) << 3u) |
                                  0x04u);

    if (superframe->sbr_flag != 0u) {
        asc[asc_length++] = 0x56u;
        asc[asc_length++] = 0xE5u;
        asc[asc_length] = (uint8_t)(0x80u | (dabplus_extension_sample_rate_index(superframe) << 3u));
        if (superframe->ps_flag != 0u) {
            asc[asc_length] = (uint8_t)(asc[asc_length] | 0x05u);
        }
        asc_length++;
        if (superframe->ps_flag != 0u) {
            asc[asc_length++] = 0x48u;
            asc[asc_length++] = 0x80u;
        }
    }

    return asc_length;
}

void dab_audio_playback_close(dab_audio_playback_t *audio) {
    if (audio == 0) {
        return;
    }
    dab_audio_playback_stop_thread(audio);
    dab_audio_ring_deinit(audio);
#if defined(DAB_HAVE_FDK_AAC)
    if (audio->decoder != 0) {
        aacDecoder_Close(audio->decoder);
        audio->decoder = 0;
    }
#endif
#if defined(DAB_HAVE_FAAD2)
    if (audio->faad_decoder != 0) {
        NeAACDecClose(audio->faad_decoder);
        audio->faad_decoder = 0;
    }
#endif
    memset(audio, 0, sizeof(*audio));
}

static void dab_audio_playback_reset_decoder(dab_audio_playback_t *audio) {
    if (audio == 0) {
        return;
    }
    dab_audio_playback_stop_thread(audio);
    dab_audio_ring_deinit(audio);
#if defined(DAB_HAVE_FDK_AAC)
    if (audio->decoder != 0) {
        aacDecoder_Close(audio->decoder);
        audio->decoder = 0;
    }
#endif
#if defined(DAB_HAVE_FAAD2)
    if (audio->faad_decoder != 0) {
        NeAACDecClose(audio->faad_decoder);
        audio->faad_decoder = 0;
    }
#endif
    audio->decoder_backend = DAB_AUDIO_DECODER_NONE;
    audio->sample_rate = 0u;
    audio->channels = 0u;
    dab_audio_eq_init(&audio->eq);
    audio->eq_gain_version = 0u;
}

static int dab_audio_playback_open_stream(dab_audio_playback_t *audio,
                                          uint32_t sample_rate,
                                          uint8_t channels) {
    unsigned int ring_seconds;
    size_t ring_frames;
    int output_failed;

    if (audio == 0 || sample_rate == 0u || channels == 0u) {
        return -1;
    }
    if (audio->output_ready != 0u &&
        audio->sample_rate == sample_rate &&
        audio->channels == channels) {
        return 0;
    }
    if (audio->thread_running != 0u) {
        dab_audio_playback_stop_thread(audio);
    }
    dab_audio_ring_deinit(audio);

    audio->sample_rate = sample_rate;
    audio->channels = channels;
    dab_audio_eq_init(&audio->eq);
    if (dab_audio_eq_configure(&audio->eq, sample_rate, channels) != 0) {
        fprintf(stderr, "audio playback: eq init failed\n");
        return -1;
    }
    audio->eq_gain_version = 0u;
    dab_audio_playback_sync_equalizer(audio);
    dab_audio_spectrum_init(&audio->spectrum, sample_rate, channels);
    ring_seconds = dab_audio_env_u32("DAB_MSC_REF_COMPARE_AUDIO_RING_SECONDS", DAB_AUDIO_RING_SECONDS);
    ring_frames = (size_t)sample_rate * ring_seconds;
    if (ring_frames < (size_t)DAB_AUDIO_FRAMES_PER_BUFFER * 8u) {
        ring_frames = (size_t)DAB_AUDIO_FRAMES_PER_BUFFER * 8u;
    }
    if (dab_audio_ring_init(audio, ring_frames) != 0) {
        fprintf(stderr, "audio playback: ring init failed\n");
        return -1;
    }
    audio->thread_stop = 0u;
    audio->output_ready = 0u;
    audio->output_failed = 0u;
    audio->prebuffer_done = 0u;
    if (dab_thread_create(&audio->thread, dab_audio_playback_thread_main, audio, "dab_audio_playback") != 0) {
        fprintf(stderr, "audio playback: failed to create playback thread\n");
        dab_audio_ring_deinit(audio);
        return -1;
    }
    audio->thread_running = 1u;
    dab_mutex_lock(&audio->ring_mutex);
    while (audio->output_ready == 0u && audio->output_failed == 0u) {
        dab_cond_wait(&audio->ring_cond, &audio->ring_mutex);
    }
    output_failed = audio->output_failed != 0u;
    dab_mutex_unlock(&audio->ring_mutex);
    if (output_failed) {
        (void)dab_thread_join(&audio->thread, 0);
        audio->thread_running = 0u;
        dab_audio_ring_deinit(audio);
        audio->sample_rate = 0u;
        audio->channels = 0u;
        return -1;
    }
    return 0;
}

static int dab_audio_playback_write_stream(dab_audio_playback_t *audio,
                                           const void *pcm,
                                           unsigned long frames) {
    const int16_t *pcm_out = (const int16_t *)pcm;

    if (audio == 0 || audio->output_ready == 0u || pcm == 0 || frames == 0u) {
        return -1;
    }
    if (audio->ring_ready == 0u) {
        return -1;
    }
    dab_audio_playback_sync_equalizer(audio);
    if (audio->eq.configured != 0u) {
        if (dab_audio_playback_ensure_eq_buffer(audio, (uint32_t)frames) != 0 ||
            dab_audio_eq_process_i16(&audio->eq, audio->eq_pcm, (const int16_t *)pcm, (uint32_t)frames) != 0) {
            return -1;
        }
        pcm_out = audio->eq_pcm;
    }
    dab_audio_playback_publish_spectrum(audio, pcm_out, (uint32_t)frames);
    dab_audio_ring_write_frames(audio, pcm_out, (size_t)frames);
    return 0;
}

static int dab_audio_playback_configure(dab_audio_playback_t *audio,
                                        const dab_msc_dabplus_superframe_t *superframe) {
    uint8_t asc[7];
    uint32_t asc_length;

    if (audio == 0 || superframe == 0) {
        return -1;
    }

    asc_length = dab_audio_playback_build_asc(superframe, asc, sizeof(asc));
    if (asc_length == 0u) {
        fprintf(stderr, "audio playback: failed to build ASC\n");
        return -1;
    }

#if defined(DAB_HAVE_FAAD2)
    if (audio->decoder_backend == DAB_AUDIO_DECODER_NONE) {
        NeAACDecHandle decoder = NeAACDecOpen();
        if (decoder != 0) {
            NeAACDecConfigurationPtr config = NeAACDecGetCurrentConfiguration(decoder);
            if (config != 0) {
                unsigned long sr = 0u;
                unsigned char ch = 0u;

                config->outputFormat = FAAD_FMT_16BIT;
                config->dontUpSampleImplicitSBR = 0u;
                if (NeAACDecSetConfiguration(decoder, config) != 0u &&
                    NeAACDecInit2(decoder, asc, (unsigned long)asc_length, &sr, &ch) == 0) {
                    audio->faad_decoder = decoder;
                    audio->decoder_backend = DAB_AUDIO_DECODER_FAAD;
                    audio->sample_rate = (uint32_t)sr;
                    audio->channels = (uint8_t)ch;
                } else {
                    NeAACDecClose(decoder);
                }
            } else {
                NeAACDecClose(decoder);
            }
        }
    }
#endif
#if defined(DAB_HAVE_FDK_AAC)
    if (audio->decoder_backend == DAB_AUDIO_DECODER_NONE) {
        HANDLE_AACDECODER decoder = aacDecoder_Open(TT_MP4_RAW, 1u);
        if (decoder != 0) {
            UCHAR *asc_ptr = asc;
            UINT asc_size = (UINT)asc_length;
            AAC_DECODER_ERROR err = aacDecoder_ConfigRaw(decoder, &asc_ptr, &asc_size);

            if (err == AAC_DEC_OK) {
                audio->decoder = decoder;
                audio->decoder_backend = DAB_AUDIO_DECODER_FDK;
                audio->sample_rate = 0u;
                audio->channels = 0u;
            } else {
                if (audio->debug_enabled != 0u) {
                    fprintf(stderr, "audio playback: aacDecoder_ConfigRaw failed (%d)\n", (int)err);
                }
                aacDecoder_Close(decoder);
            }
        }
    }
#endif
    if (audio->decoder_backend == DAB_AUDIO_DECODER_NONE) {
        fprintf(stderr, "audio playback: no usable AAC decoder backend\n");
        audio->configured = 0u;
        return -1;
    }

    audio->configured = 1u;
    audio->dac_rate = superframe->dac_rate;
    audio->sbr_flag = superframe->sbr_flag;
    audio->aac_channel_mode = superframe->aac_channel_mode;
    audio->ps_flag = superframe->ps_flag;
    audio->mpeg_surround_config = superframe->mpeg_surround_config;
    printf("audio playback decoder configured sid=0x%08x backend=%s sbr=%u ps=%u dac_rate=%u\n",
           superframe->sid,
           audio->decoder_backend == DAB_AUDIO_DECODER_FAAD ? "faad" :
           (audio->decoder_backend == DAB_AUDIO_DECODER_FDK ? "fdk" : "none"),
           audio->sbr_flag,
           audio->ps_flag,
           audio->dac_rate);
    if (audio->debug_enabled != 0u) {
        printf("audio playback superframe sid=0x%08x generation=%llu bytes=%u au_count=%u channel_mode=%u mpeg_surround=%u\n",
               superframe->sid,
               (unsigned long long)superframe->generation,
               superframe->superframe_bytes,
               superframe->au_count,
               superframe->aac_channel_mode,
               superframe->mpeg_surround_config);
        dab_audio_playback_print_hex("audio playback ASC", asc, asc_length, asc_length);
    }
    return 0;
}

static int dab_audio_playback_ensure_configured(dab_audio_playback_t *audio,
                                                const dab_msc_dabplus_superframe_t *superframe) {
    if (audio == 0 || superframe == 0) {
        return -1;
    }
    if (audio->decoder_backend != DAB_AUDIO_DECODER_NONE &&
        audio->output_ready != 0u &&
        audio->dac_rate == superframe->dac_rate &&
        audio->sbr_flag == superframe->sbr_flag &&
        audio->aac_channel_mode == superframe->aac_channel_mode &&
        audio->ps_flag == superframe->ps_flag &&
        audio->mpeg_surround_config == superframe->mpeg_surround_config) {
        return 0;
    }

    if (audio->decoder_backend != DAB_AUDIO_DECODER_NONE) {
        dab_audio_playback_reset_decoder(audio);
    }
    return dab_audio_playback_configure(audio, superframe);
}

static int dab_audio_playback_decode_au(dab_audio_playback_t *audio,
                                        const uint8_t *au_data,
                                        uint32_t au_length) {
    if (audio == 0 || audio->decoder_backend == DAB_AUDIO_DECODER_NONE || au_data == 0 || au_length == 0u) {
        fprintf(stderr,
                "audio playback: invalid AU decode state backend=%u data=%p au_length=%u\n",
                audio != 0 ? audio->decoder_backend : 0u,
                (const void *)au_data,
                au_length);
        return -1;
    }
#if defined(DAB_HAVE_FAAD2)
    if (audio->decoder_backend == DAB_AUDIO_DECODER_FAAD) {
        NeAACDecFrameInfo info;
        void *pcm;

        memset(&info, 0, sizeof(info));
        pcm = NeAACDecDecode(audio->faad_decoder, &info, (unsigned char *)au_data, (unsigned long)au_length);
        if (info.bytesconsumed == 0u && info.samples == 0u) {
            fprintf(stderr, "audio playback: NeAACDecDecode failed (%u)\n", info.error);
            if (audio->debug_enabled != 0u) {
                dab_audio_playback_print_hex("audio playback failing AU", au_data, au_length, 32u);
            }
            return 1;
        }
        if (info.samples == 0u || pcm == 0 || info.channels == 0u || info.samplerate == 0u) {
            return 0;
        }
        if (dab_audio_playback_open_stream(audio, (uint32_t)info.samplerate, (uint8_t)info.channels) != 0) {
            return -1;
        }
        if (dab_audio_playback_write_stream(audio, pcm, (unsigned long)(info.samples / info.channels)) != 0) {
            return -1;
        }
        return 0;
    }
#endif
#if defined(DAB_HAVE_FDK_AAC)
    if (audio->decoder_backend == DAB_AUDIO_DECODER_FDK) {
        UCHAR *in_ptr = (UCHAR *)au_data;
        UINT buffer_size = au_length;
        UINT bytes_valid = au_length;
        AAC_DECODER_ERROR err;
        CStreamInfo *info;
        unsigned long frames;

        err = aacDecoder_Fill(audio->decoder, &in_ptr, &buffer_size, &bytes_valid);
        if (err != AAC_DEC_OK) {
            fprintf(stderr, "audio playback: aacDecoder_Fill failed (%d)\n", (int)err);
            return -1;
        }
        err = aacDecoder_DecodeFrame(audio->decoder,
                                     audio->pcm,
                                     (INT)(sizeof(audio->pcm) / sizeof(audio->pcm[0])),
                                     0u);
        if (err != AAC_DEC_OK) {
            fprintf(stderr, "audio playback: aacDecoder_DecodeFrame failed (%d)\n", (int)err);
            if (audio->debug_enabled != 0u) {
                dab_audio_playback_print_hex("audio playback failing AU", au_data, au_length, 32u);
            }
            return 1;
        }

        info = aacDecoder_GetStreamInfo(audio->decoder);
        if (info == 0 || info->frameSize <= 0 || info->numChannels <= 0) {
            fprintf(stderr, "audio playback: invalid decoder stream info\n");
            return -1;
        }
        if (dab_audio_playback_open_stream(audio,
                                           (uint32_t)info->sampleRate,
                                           (uint8_t)info->numChannels) != 0) {
            return -1;
        }
        frames = (unsigned long)info->frameSize;
        if (dab_audio_playback_write_stream(audio, audio->pcm, frames) != 0) {
            return -1;
        }
        return 0;
    }
#endif
    return -1;
}

static const char *dab_audio_playback_error_string(const dab_audio_playback_t *audio, int error_code) {
#if defined(DAB_HAVE_FAAD2)
    if (audio != 0 && audio->decoder_backend == DAB_AUDIO_DECODER_FAAD) {
        return NeAACDecGetErrorMessage((unsigned char)error_code);
    }
#endif
    (void)audio;
    (void)error_code;
    return "";
}

int dab_audio_playback_consume_superframe(dab_audio_playback_t *audio,
                                          const dab_msc_dabplus_superframe_t *superframe) {
    uint32_t i;

    if (audio == 0 || superframe == 0 || superframe->valid == 0u || superframe->synchronized == 0u) {
        fprintf(stderr,
                "audio playback: invalid superframe valid=%u sync=%u generation=%llu au_count=%u\n",
                superframe != 0 ? superframe->valid : 0u,
                superframe != 0 ? superframe->synchronized : 0u,
                (unsigned long long)(superframe != 0 ? superframe->generation : 0u),
                superframe != 0 ? superframe->au_count : 0u);
        return -1;
    }
    if (dab_audio_playback_ensure_configured(audio, superframe) != 0) {
        return -1;
    }
    for (i = 0u; i < superframe->au_count; ++i) {
        const uint32_t au_start = superframe->au_start[i];
        const uint32_t au_length = superframe->au_length[i];

        if ((au_start + au_length) > superframe->superframe_bytes) {
            fprintf(stderr, "audio playback: AU %u exceeds superframe bounds\n", i);
            return -1;
        }
        if (au_length == 0u) {
            fprintf(stderr,
                    "audio playback: AU %u has zero payload length start=%u superframe_bytes=%u au_count=%u\n",
                    i,
                    au_start,
                    superframe->superframe_bytes,
                    superframe->au_count);
            return -1;
        }
        if (audio->debug_enabled != 0u) {
            const uint32_t crc_offset = au_start + au_length;
            printf("audio playback AU %u start=%u length=%u",
                   i,
                   au_start,
                   au_length);
            if ((crc_offset + 1u) < superframe->superframe_bytes) {
                printf(" crc=0x%02x%02x",
                       superframe->superframe_data[crc_offset],
                       superframe->superframe_data[crc_offset + 1u]);
            }
            printf("\n");
        }
        {
            int decode_rc = dab_audio_playback_decode_au(audio, superframe->superframe_data + au_start, au_length);

            if (decode_rc < 0) {
                return -1;
            }
            if (decode_rc > 0) {
                audio->total_decode_failures++;
                audio->consecutive_decode_failures++;
                fprintf(stderr,
                        "audio playback: AU %u decode failure sid=0x%08x generation=%llu consecutive=%u total=%u msg=%s\n",
                        i,
                        superframe->sid,
                        (unsigned long long)superframe->generation,
                        audio->consecutive_decode_failures,
                        audio->total_decode_failures,
                        dab_audio_playback_error_string(audio, decode_rc));
                continue;
            }
        }
    }
    audio->consecutive_decode_failures = 0u;
    audio->last_generation = superframe->generation;
    return 0;
}

void dab_audio_playback_note_missing_superframe(dab_audio_playback_t *audio,
                                                uint32_t sid,
                                                uint32_t msc_frames_seen) {
    if (audio == 0) {
        return;
    }
    if (audio->missing_superframe_reports >= 40u) {
        return;
    }
    audio->missing_superframe_reports++;
    fprintf(stderr,
            "audio playback: sid=0x%08x has no synchronized DAB+ superframe yet (msc_frames=%u)\n",
            sid,
            msc_frames_seen);
}

void dab_audio_playback_report_dabplus_debug(dab_audio_playback_t *audio,
                                             uint32_t sid,
                                             const dab_msc_dabplus_debug_t *debug) {
    if (audio == 0 || debug == 0) {
        return;
    }
    if (audio->missing_superframe_reports >= 40u) {
        return;
    }
    audio->missing_superframe_reports++;
    fprintf(stderr,
            "audio playback: sid=0x%08x subch=%u cif_bytes=%u columns=%u sync=%u superframe_valid=%u generation=%llu attempts=%u failures=%u rs_last=%u rs_total=%u last_ok=%u resets=%u reason=0x%02x\n",
            sid,
            debug->subchid,
            debug->cif_bytes,
            debug->columns_filled,
            debug->synchronized,
            debug->superframe_valid,
            (unsigned long long)debug->generation,
            debug->decode_attempts,
            debug->decode_failures,
            debug->rs_corrected_last_superframe,
            debug->rs_corrected_total,
            debug->last_decode_ok,
            debug->runtime_reset_count,
            debug->last_reconfig_reason);
}
#endif
