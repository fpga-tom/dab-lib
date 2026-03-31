#ifndef DAB_TOOLS_AUDIO_PLAYBACK_H
#define DAB_TOOLS_AUDIO_PLAYBACK_H

#include <stdint.h>

#include "dab/config.h"
#include "dab/core/msc.h"

#if defined(DAB_HAVE_AUDIO_PLAYBACK)
#if defined(DAB_HAVE_FDK_AAC)
#include <fdk-aac/aacdecoder_lib.h>
#endif
#if defined(DAB_HAVE_FAAD2)
#include <neaacdec.h>
#endif
#include <pthread.h>
#include <portaudio.h>

enum {
    DAB_AUDIO_DECODER_NONE = 0,
    DAB_AUDIO_DECODER_FAAD = 1,
    DAB_AUDIO_DECODER_FDK = 2
};

typedef struct {
#if defined(DAB_HAVE_FDK_AAC)
    HANDLE_AACDECODER decoder;
    INT_PCM pcm[8192];
#endif
#if defined(DAB_HAVE_FAAD2)
    NeAACDecHandle faad_decoder;
#endif
    PaStream *stream;
    uint8_t decoder_backend;
    uint8_t configured;
    uint8_t debug_enabled;
    uint8_t dac_rate;
    uint8_t sbr_flag;
    uint8_t aac_channel_mode;
    uint8_t ps_flag;
    uint8_t mpeg_surround_config;
    uint32_t sample_rate;
    uint8_t channels;
    int16_t *ring;
    size_t ring_frames;
    size_t ring_read;
    size_t ring_write;
    size_t ring_count;
    pthread_mutex_t ring_mutex;
    pthread_cond_t ring_cond;
    pthread_t thread;
    uint8_t ring_ready;
    uint8_t thread_running;
    uint8_t thread_stop;
    uint8_t prebuffer_done;
    uint64_t last_generation;
    uint32_t missing_superframe_reports;
    uint32_t consecutive_decode_failures;
    uint32_t total_decode_failures;
} dab_audio_playback_t;

void dab_audio_playback_close(dab_audio_playback_t *audio);
int dab_audio_playback_consume_superframe(dab_audio_playback_t *audio,
                                          const dab_msc_dabplus_superframe_t *superframe);
void dab_audio_playback_note_missing_superframe(dab_audio_playback_t *audio,
                                                uint32_t sid,
                                                uint32_t msc_frames_seen);
void dab_audio_playback_report_dabplus_debug(dab_audio_playback_t *audio,
                                             uint32_t sid,
                                             const dab_msc_dabplus_debug_t *debug);
#endif

#endif
