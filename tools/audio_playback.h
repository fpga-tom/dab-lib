#ifndef DAB_TOOLS_AUDIO_PLAYBACK_H
#define DAB_TOOLS_AUDIO_PLAYBACK_H

#include <stdint.h>

#include "dab/config.h"
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
#include "dab/os/os_mutex.h"
#include <portaudio.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    DAB_AUDIO_DECODER_NONE = 0,
    DAB_AUDIO_DECODER_FAAD = 1,
    DAB_AUDIO_DECODER_FDK = 2
};

typedef struct {
    uint8_t valid;
    uint8_t decoder_backend;
    uint8_t channels;
    uint8_t output_active;
    uint32_t sample_rate;
    uint64_t pcm_frames_written;
    uint64_t output_frames;
    uint64_t muted_frames;
    uint64_t underrun_events;
    uint64_t longest_interruption_frames;
    uint64_t clipped_samples;
    uint64_t total_samples;
    float left_rms_dbfs;
    float right_rms_dbfs;
    float left_peak_dbfs;
    float right_peak_dbfs;
} dab_audio_playback_diagnostics_t;

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
    dab_mutex_t ring_mutex;
    uint8_t ring_ready;
    uint8_t prebuffer_done;
    uint64_t last_generation;
    uint32_t missing_superframe_reports;
    uint32_t consecutive_decode_failures;
    uint32_t total_decode_failures;
    uint32_t service_id;
    uint8_t asc[8];
    uint32_t asc_length;
    dab_audio_eq_t eq;
    int16_t *eq_pcm;
    uint32_t eq_pcm_frames;
    uint64_t eq_gain_version;
    dab_audio_spectrum_analyzer_t spectrum;
    uint64_t diagnostic_pcm_frames_written;
    uint64_t diagnostic_output_frames;
    uint64_t diagnostic_muted_frames;
    uint64_t diagnostic_underrun_events;
    uint64_t diagnostic_longest_interruption_frames;
    uint64_t diagnostic_clipped_samples;
    uint64_t diagnostic_total_samples;
    float diagnostic_left_rms_dbfs;
    float diagnostic_right_rms_dbfs;
    float diagnostic_left_peak_dbfs;
    float diagnostic_right_peak_dbfs;
    uint8_t diagnostic_level_valid;
} dab_audio_playback_t;

typedef void (*dab_audio_playback_spectrum_callback_t)(const float *bins,
                                                       uint32_t bin_count,
                                                       uint32_t sample_rate,
                                                       void *user);

void dab_audio_playback_set_spectrum_callback(dab_audio_playback_spectrum_callback_t callback,
                                              void *user);
void dab_audio_playback_set_equalizer_gains(const float *gains_db, uint32_t count);
void dab_audio_playback_close(dab_audio_playback_t *audio);
int dab_audio_playback_output_active(dab_audio_playback_t *audio);
int dab_audio_playback_get_diagnostics(
    dab_audio_playback_t *audio,
    dab_audio_playback_diagnostics_t *diagnostics);
int dab_audio_playback_set_format(dab_audio_playback_t *audio,
                                  uint32_t service_id,
                                  uint32_t sample_rate,
                                  uint32_t channels,
                                  const uint8_t *asc,
                                  uint32_t asc_length);
int dab_audio_playback_consume_au(dab_audio_playback_t *audio,
                                  const uint8_t *au_data,
                                  uint32_t au_length,
                                  uint64_t generation);
int dab_audio_playback_consume_superframe(dab_audio_playback_t *audio,
                                          const dab_msc_dabplus_superframe_t *superframe);
void dab_audio_playback_note_missing_superframe(dab_audio_playback_t *audio,
                                                uint32_t sid,
                                                uint32_t msc_frames_seen);
void dab_audio_playback_report_dabplus_debug(dab_audio_playback_t *audio,
                                             uint32_t sid,
                                             const dab_msc_dabplus_debug_t *debug);

#ifdef __cplusplus
}
#endif
#endif

#endif
