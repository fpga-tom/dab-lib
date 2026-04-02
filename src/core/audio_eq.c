#include "dab/core/audio_eq.h"

#include <math.h>
#include <string.h>

#include "prs_fft_backend.h"

static const float kTwoPi = 6.28318530717958647692f;
static const float kBandFrequencies[DAB_AUDIO_EQ_BAND_COUNT] = {
    60.0f, 170.0f, 310.0f, 600.0f, 1000.0f,
    3000.0f, 6000.0f, 10000.0f, 12000.0f, 16000.0f
};

static const dab_prs_fft_backend_t *dab_audio_eq_select_backend(void) {
#if defined(DAB_ENABLE_AVX2) && (defined(__AVX2__) || defined(_M_AVX2))
    return dab_prs_fft_avx2_backend();
#elif defined(DAB_ENABLE_NEON) && (defined(__ARM_NEON) || defined(__ARM_NEON__))
    return dab_prs_fft_neon_backend();
#else
    return dab_prs_fft_scalar_backend();
#endif
}

static void dab_audio_eq_bit_reverse(const dab_prs_fft_t *fft, float *re, float *im) {
    uint32_t i;

    for (i = 0u; i < fft->config.fft_samples; ++i) {
        const uint32_t j = fft->bitrev[i];

        if (j > i) {
            const float tr = re[i];
            const float ti = im[i];
            re[i] = re[j];
            im[i] = im[j];
            re[j] = tr;
            im[j] = ti;
        }
    }
}

static void dab_audio_eq_fft_execute(const dab_audio_eq_t *eq, float *re, float *im, int inverse) {
    const dab_prs_fft_backend_t *backend = dab_audio_eq_select_backend();
    uint32_t stage;
    uint32_t i;

    if (inverse != 0) {
        for (i = 0u; i < DAB_AUDIO_EQ_FFT_SIZE; ++i) {
            im[i] = -im[i];
        }
    }

    dab_audio_eq_bit_reverse(&eq->fft, re, im);
    for (stage = 0u; stage < eq->fft.stage_count; ++stage) {
        backend->stage_fn(re,
                          im,
                          eq->fft.twiddle_re + eq->fft.stage_offsets[stage],
                          eq->fft.twiddle_im + eq->fft.stage_offsets[stage],
                          DAB_AUDIO_EQ_FFT_SIZE,
                          eq->fft.stage_halves[stage]);
    }

    if (inverse != 0) {
        const float scale = 1.0f / (float)DAB_AUDIO_EQ_FFT_SIZE;

        for (i = 0u; i < DAB_AUDIO_EQ_FFT_SIZE; ++i) {
            re[i] *= scale;
            im[i] = -im[i] * scale;
        }
    }
}

static float dab_audio_eq_interpolate_gain_db(const dab_audio_eq_t *eq, float frequency_hz) {
    uint32_t i;

    if (frequency_hz <= kBandFrequencies[0]) {
        return eq->gains_db[0];
    }
    if (frequency_hz >= kBandFrequencies[DAB_AUDIO_EQ_BAND_COUNT - 1u]) {
        return eq->gains_db[DAB_AUDIO_EQ_BAND_COUNT - 1u];
    }

    for (i = 0u; i + 1u < DAB_AUDIO_EQ_BAND_COUNT; ++i) {
        if (frequency_hz <= kBandFrequencies[i + 1u]) {
            const float log_f0 = logf(kBandFrequencies[i]);
            const float log_f1 = logf(kBandFrequencies[i + 1u]);
            const float t = (logf(frequency_hz) - log_f0) / (log_f1 - log_f0);

            return eq->gains_db[i] + ((eq->gains_db[i + 1u] - eq->gains_db[i]) * t);
        }
    }

    return eq->gains_db[DAB_AUDIO_EQ_BAND_COUNT - 1u];
}

static void dab_audio_eq_rebuild_response(dab_audio_eq_t *eq) {
    const float delay = (float)DAB_AUDIO_EQ_HOP_SIZE;
    uint32_t k;

    memset(eq->filter_re, 0, sizeof(eq->filter_re));
    memset(eq->filter_im, 0, sizeof(eq->filter_im));

    for (k = 0u; k <= (DAB_AUDIO_EQ_FFT_SIZE / 2u); ++k) {
        const float frequency_hz = ((float)k * (float)eq->sample_rate) / (float)DAB_AUDIO_EQ_FFT_SIZE;
        const float gain_db = dab_audio_eq_interpolate_gain_db(eq, frequency_hz);
        const float magnitude = powf(10.0f, gain_db / 20.0f);
        const float phase = -(kTwoPi * delay * (float)k) / (float)DAB_AUDIO_EQ_FFT_SIZE;
        const float re = magnitude * cosf(phase);
        const float im = magnitude * sinf(phase);

        eq->filter_re[k] = re;
        eq->filter_im[k] = im;
        if (k != 0u && k != (DAB_AUDIO_EQ_FFT_SIZE / 2u)) {
            eq->filter_re[DAB_AUDIO_EQ_FFT_SIZE - k] = re;
            eq->filter_im[DAB_AUDIO_EQ_FFT_SIZE - k] = -im;
        }
    }

    eq->response_dirty = 0u;
}

static void dab_audio_eq_output_push(dab_audio_eq_t *eq, const float *frame) {
    uint32_t ch;

    if (eq->output_count >= DAB_AUDIO_EQ_OUTPUT_CAPACITY) {
        eq->output_read = (eq->output_read + 1u) % DAB_AUDIO_EQ_OUTPUT_CAPACITY;
        eq->output_count--;
    }

    for (ch = 0u; ch < eq->channels; ++ch) {
        eq->output_ring[ch][eq->output_write] = frame[ch];
    }
    eq->output_write = (eq->output_write + 1u) % DAB_AUDIO_EQ_OUTPUT_CAPACITY;
    eq->output_count++;
}

static void dab_audio_eq_process_block(dab_audio_eq_t *eq) {
    float block_output[DAB_AUDIO_EQ_MAX_CHANNELS][DAB_AUDIO_EQ_HOP_SIZE];
    uint32_t ch;

    if (eq->response_dirty != 0u) {
        dab_audio_eq_rebuild_response(eq);
    }

    for (ch = 0u; ch < eq->channels; ++ch) {
        uint32_t i;

        memcpy(eq->work_re, eq->history[ch], DAB_AUDIO_EQ_HOP_SIZE * sizeof(float));
        memcpy(eq->work_re + DAB_AUDIO_EQ_HOP_SIZE, eq->pending[ch], DAB_AUDIO_EQ_HOP_SIZE * sizeof(float));
        memset(eq->work_im, 0, sizeof(eq->work_im));

        dab_audio_eq_fft_execute(eq, eq->work_re, eq->work_im, 0);

        for (i = 0u; i < DAB_AUDIO_EQ_FFT_SIZE; ++i) {
            const float in_re = eq->work_re[i];
            const float in_im = eq->work_im[i];
            const float h_re = eq->filter_re[i];
            const float h_im = eq->filter_im[i];

            eq->work_re[i] = (in_re * h_re) - (in_im * h_im);
            eq->work_im[i] = (in_re * h_im) + (in_im * h_re);
        }

        dab_audio_eq_fft_execute(eq, eq->work_re, eq->work_im, 1);

        for (i = 0u; i < DAB_AUDIO_EQ_HOP_SIZE; ++i) {
            block_output[ch][i] = eq->work_re[DAB_AUDIO_EQ_HOP_SIZE + i];
        }
        memcpy(eq->history[ch], eq->pending[ch], DAB_AUDIO_EQ_HOP_SIZE * sizeof(float));
    }

    for (uint32_t i = 0u; i < DAB_AUDIO_EQ_HOP_SIZE; ++i) {
        float frame[DAB_AUDIO_EQ_MAX_CHANNELS] = {0};

        for (ch = 0u; ch < eq->channels; ++ch) {
            frame[ch] = block_output[ch][i];
        }
        dab_audio_eq_output_push(eq, frame);
    }
}

void dab_audio_eq_init(dab_audio_eq_t *eq) {
    if (eq == 0) {
        return;
    }
    memset(eq, 0, sizeof(*eq));
}

int dab_audio_eq_configure(dab_audio_eq_t *eq, uint32_t sample_rate, uint8_t channels) {
    dab_prs_fft_config_t fft_config;

    if (eq == 0 || sample_rate == 0u || channels == 0u || channels > DAB_AUDIO_EQ_MAX_CHANNELS) {
        return -1;
    }

    memset(eq->history, 0, sizeof(eq->history));
    memset(eq->pending, 0, sizeof(eq->pending));
    memset(eq->output_ring, 0, sizeof(eq->output_ring));
    eq->pending_count = 0u;
    eq->output_read = 0u;
    eq->output_write = 0u;
    eq->output_count = 0u;
    eq->sample_rate = sample_rate;
    eq->channels = channels;
    eq->configured = 0u;
    eq->response_dirty = 1u;

    fft_config.fft_samples = DAB_AUDIO_EQ_FFT_SIZE;
    fft_config.sample_rate = sample_rate;
    if (dab_prs_fft_init(&eq->fft, &fft_config) != 0) {
        return -1;
    }

    eq->configured = 1u;
    return 0;
}

void dab_audio_eq_set_gains_db(dab_audio_eq_t *eq, const float *gains_db, uint32_t count) {
    uint32_t i;

    if (eq == 0) {
        return;
    }

    for (i = 0u; i < DAB_AUDIO_EQ_BAND_COUNT; ++i) {
        eq->gains_db[i] = (gains_db != 0 && i < count) ? gains_db[i] : 0.0f;
    }
    eq->response_dirty = 1u;
}

int dab_audio_eq_process_i16(dab_audio_eq_t *eq, int16_t *dst, const int16_t *src, uint32_t frames) {
    uint32_t frame;

    if (eq == 0 || dst == 0 || src == 0 || frames == 0u || eq->configured == 0u) {
        return -1;
    }

    for (frame = 0u; frame < frames; ++frame) {
        uint32_t ch;

        for (ch = 0u; ch < eq->channels; ++ch) {
            eq->pending[ch][eq->pending_count] = (float)src[(frame * eq->channels) + ch];
        }
        eq->pending_count++;

        if (eq->pending_count == DAB_AUDIO_EQ_HOP_SIZE) {
            dab_audio_eq_process_block(eq);
            eq->pending_count = 0u;
        }

        if (eq->output_count != 0u) {
            for (ch = 0u; ch < eq->channels; ++ch) {
                float sample = eq->output_ring[ch][eq->output_read];

                if (sample > 32767.0f) {
                    sample = 32767.0f;
                } else if (sample < -32768.0f) {
                    sample = -32768.0f;
                }
                dst[(frame * eq->channels) + ch] = (int16_t)lroundf(sample);
            }
            eq->output_read = (eq->output_read + 1u) % DAB_AUDIO_EQ_OUTPUT_CAPACITY;
            eq->output_count--;
        } else {
            for (ch = 0u; ch < eq->channels; ++ch) {
                dst[(frame * eq->channels) + ch] = 0;
            }
        }
    }

    return 0;
}
