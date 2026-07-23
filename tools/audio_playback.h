#ifndef DAB_TOOLS_AUDIO_PLAYBACK_H
#define DAB_TOOLS_AUDIO_PLAYBACK_H

#include <stdint.h>

#include "dab/config.h"
#include "dab/audio/audio_output.h"
#include "dab/core/audio_eq.h"
#include "dab/core/audio_spectrum.h"
#include "dab/core/msc.h"

#if defined(DAB_HAVE_AUDIO_PLAYBACK)
#if defined(DAB_HAVE_FDK_AAC)
#include <fdk-aac/aacdecoder_lib.h>
#endif
#if defined(DAB_HAVE_FAAD2)
#include <neaacdec.h>
#endif
#include "dab/os/os_cond.h"
#include "dab/os/os_mutex.h"
#include "dab/os/os_thread.h"

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
    dab_audio_output_t *output;
    dab_audio_output_backend_t output_backend;
    uint8_t realtime_enabled;
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
    dab_mutex_t ring_mutex;
    dab_cond_t ring_cond;
    dab_thread_t thread;
    uint8_t ring_ready;
    uint8_t thread_running;
    uint8_t thread_stop;
    uint8_t output_ready;
    uint8_t output_failed;
    uint8_t prebuffer_done;
    uint64_t last_generation;
    uint32_t missing_superframe_reports;
    uint32_t consecutive_decode_failures;
    uint32_t total_decode_failures;
    dab_audio_eq_t eq;
    int16_t *eq_pcm;
    uint32_t eq_pcm_frames;
    uint64_t eq_gain_version;
    dab_audio_spectrum_analyzer_t spectrum;
} dab_audio_playback_t;

typedef void (*dab_audio_playback_spectrum_callback_t)(const float *bins,
                                                       uint32_t bin_count,
                                                       uint32_t sample_rate,
                                                       void *user);

void dab_audio_playback_set_spectrum_callback(dab_audio_playback_spectrum_callback_t callback,
                                              void *user);
void dab_audio_playback_set_equalizer_gains(const float *gains_db, uint32_t count);
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
