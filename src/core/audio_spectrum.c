#include "dab/core/audio_spectrum.h"

#include <math.h>
#include <string.h>

#include "dab/simd/simd.h"

enum {
    DAB_AUDIO_SPECTRUM_HALF_FFT = DAB_AUDIO_SPECTRUM_FFT_SIZE / 2u
};

static const float kPi = 3.14159265358979323846f;

static uint16_t reverse_bits10(uint16_t value) {
    uint16_t reversed = 0u;
    uint32_t bit;

    for (bit = 0u; bit < 10u; ++bit) {
        reversed = (uint16_t)((reversed << 1u) | (value & 1u));
        value = (uint16_t)(value >> 1u);
    }
    return reversed;
}

static void fft_reorder(const uint16_t *bit_reverse, float *re, float *im) {
    uint32_t i;

    for (i = 0u; i < DAB_AUDIO_SPECTRUM_FFT_SIZE; ++i) {
        const uint16_t j = bit_reverse[i];

        if (j > i) {
            const float tmp_re = re[i];
            const float tmp_im = im[i];
            re[i] = re[j];
            im[i] = im[j];
            re[j] = tmp_re;
            im[j] = tmp_im;
        }
    }
}

static void fft_execute(float *re,
                        float *im,
                        const float *twiddle_re,
                        const float *twiddle_im,
                        const uint16_t *bit_reverse) {
    uint32_t len;

    fft_reorder(bit_reverse, re, im);

    for (len = 2u; len <= DAB_AUDIO_SPECTRUM_FFT_SIZE; len <<= 1u) {
        const uint32_t half = len >> 1u;
        const uint32_t twiddle_step = DAB_AUDIO_SPECTRUM_FFT_SIZE / len;
        uint32_t base;

        for (base = 0u; base < DAB_AUDIO_SPECTRUM_FFT_SIZE; base += len) {
            uint32_t j;

            for (j = 0u; j < half; ++j) {
                const uint32_t even = base + j;
                const uint32_t odd = even + half;
                const uint32_t twiddle_index = j * twiddle_step;
                const float wr = twiddle_re[twiddle_index];
                const float wi = twiddle_im[twiddle_index];
                const float odd_re = re[odd];
                const float odd_im = im[odd];
                const float tr = (wr * odd_re) - (wi * odd_im);
                const float ti = (wr * odd_im) + (wi * odd_re);
                const float even_re = re[even];
                const float even_im = im[even];

                re[even] = even_re + tr;
                im[even] = even_im + ti;
                re[odd] = even_re - tr;
                im[odd] = even_im - ti;
            }
        }
    }
}

static void build_display_bins(dab_audio_spectrum_analyzer_t *analyzer) {
    const float min_hz = 20.0f;
    const float max_hz = 20000.0f;
    const float min_db = -70.0f;
    const float epsilon = 1.0e-20f;
    const float fft_ref = 32768.0f * (float)DAB_AUDIO_SPECTRUM_FFT_SIZE * 0.5f;
    const float ref_power = fft_ref * fft_ref;
    const float bin_hz = analyzer->sample_rate != 0u
                             ? (float)analyzer->sample_rate / (float)DAB_AUDIO_SPECTRUM_FFT_SIZE
                             : 1.0f;
    const float min_band_hz = min_hz > bin_hz ? min_hz : bin_hz;
    const float nyquist_hz = bin_hz * (float)DAB_AUDIO_SPECTRUM_HALF_FFT;
    const float max_band_hz = max_hz < nyquist_hz ? max_hz : nyquist_hz;
    uint32_t edges[DAB_AUDIO_SPECTRUM_DISPLAY_BINS + 1u];
    uint32_t band;

    edges[0] = 1u;
    for (band = 1u; band < DAB_AUDIO_SPECTRUM_DISPLAY_BINS; ++band) {
        const float t = (float)band / (float)DAB_AUDIO_SPECTRUM_DISPLAY_BINS;
        const float edge_hz = min_band_hz * powf(max_band_hz / min_band_hz, t);
        uint32_t edge = (uint32_t)floorf((edge_hz / bin_hz) + 0.5f);
        const uint32_t min_edge = edges[band - 1u] + 1u;
        const uint32_t remaining_bands = DAB_AUDIO_SPECTRUM_DISPLAY_BINS - band;
        const uint32_t max_edge = DAB_AUDIO_SPECTRUM_HALF_FFT > remaining_bands
                                      ? DAB_AUDIO_SPECTRUM_HALF_FFT - remaining_bands
                                      : min_edge;

        if (edge < min_edge) {
            edge = min_edge;
        }
        if (edge > max_edge) {
            edge = max_edge;
        }
        edges[band] = edge;
    }
    edges[DAB_AUDIO_SPECTRUM_DISPLAY_BINS] = DAB_AUDIO_SPECTRUM_HALF_FFT;

    for (band = 0u; band < DAB_AUDIO_SPECTRUM_DISPLAY_BINS; ++band) {
        uint32_t start = edges[band];
        uint32_t end = edges[band + 1u];
        float sum = 0.0f;
        uint32_t count = 0u;
        uint32_t bin;

        if (end <= start) {
            end = start + 1u;
        }
        if (end > DAB_AUDIO_SPECTRUM_HALF_FFT) {
            end = DAB_AUDIO_SPECTRUM_HALF_FFT;
        }

        for (bin = start; bin < end; ++bin) {
            sum += analyzer->power[bin];
            count++;
        }

        if (count == 0u) {
            analyzer->display[band] = 0.0f;
            continue;
        }

        {
            const float power = sum / (float)count;
            const float dbfs = 10.0f * log10f((power / ref_power) + epsilon);
            const float normalized = (dbfs - min_db) / -min_db;

            analyzer->display[band] = normalized;
        }
    }

    for (band = 0u; band < DAB_AUDIO_SPECTRUM_DISPLAY_BINS; ++band) {
        const float clamped = analyzer->display[band] < 0.0f
                                  ? 0.0f
                                  : (analyzer->display[band] > 1.0f ? 1.0f : analyzer->display[band]);

        analyzer->smoothed[band] = analyzer->ready != 0u
                                       ? (analyzer->smoothed[band] * 0.52f) + (clamped * 0.48f)
                                       : clamped;
        analyzer->display[band] = analyzer->smoothed[band];
    }
}

static void analyze_window(dab_audio_spectrum_analyzer_t *analyzer) {
    const dab_simd_ops_t *ops = dab_simd_get_ops();
    uint32_t i;

    for (i = 0u; i < DAB_AUDIO_SPECTRUM_FFT_SIZE; ++i) {
        const uint32_t index = (analyzer->write_index + i) % DAB_AUDIO_SPECTRUM_FFT_SIZE;
        analyzer->fft_re[i] = analyzer->ring[index];
    }

    if (ops != 0 && ops->mul_f32 != 0) {
        ops->mul_f32(analyzer->fft_re, analyzer->fft_re, analyzer->window, DAB_AUDIO_SPECTRUM_FFT_SIZE);
    }
    memset(analyzer->fft_im, 0, sizeof(analyzer->fft_im));

    fft_execute(analyzer->fft_re,
                analyzer->fft_im,
                analyzer->twiddle_re,
                analyzer->twiddle_im,
                analyzer->bit_reverse);

    if (ops != 0 && ops->complex_abs2_f32 != 0) {
        ops->complex_abs2_f32(analyzer->power,
                              analyzer->fft_re,
                              analyzer->fft_im,
                              DAB_AUDIO_SPECTRUM_HALF_FFT);
    } else {
        for (i = 0u; i < DAB_AUDIO_SPECTRUM_HALF_FFT; ++i) {
            analyzer->power[i] =
                (analyzer->fft_re[i] * analyzer->fft_re[i]) +
                (analyzer->fft_im[i] * analyzer->fft_im[i]);
        }
    }

    build_display_bins(analyzer);
    analyzer->ready = 1u;
}

void dab_audio_spectrum_init(dab_audio_spectrum_analyzer_t *analyzer,
                             uint32_t sample_rate,
                             uint8_t channels) {
    uint32_t i;

    if (analyzer == 0) {
        return;
    }

    memset(analyzer, 0, sizeof(*analyzer));
    analyzer->sample_rate = sample_rate;
    analyzer->channels = channels;

    for (i = 0u; i < DAB_AUDIO_SPECTRUM_FFT_SIZE; ++i) {
        const float phase = (2.0f * kPi * (float)i) / (float)(DAB_AUDIO_SPECTRUM_FFT_SIZE - 1u);
        analyzer->window[i] = 0.5f - (0.5f * cosf(phase));
        analyzer->bit_reverse[i] = reverse_bits10((uint16_t)i);
    }

    for (i = 0u; i < DAB_AUDIO_SPECTRUM_HALF_FFT; ++i) {
        const float angle = (-2.0f * kPi * (float)i) / (float)DAB_AUDIO_SPECTRUM_FFT_SIZE;
        analyzer->twiddle_re[i] = cosf(angle);
        analyzer->twiddle_im[i] = sinf(angle);
    }
}

void dab_audio_spectrum_reset(dab_audio_spectrum_analyzer_t *analyzer,
                              uint32_t sample_rate,
                              uint8_t channels) {
    float window[DAB_AUDIO_SPECTRUM_FFT_SIZE];
    float twiddle_re[DAB_AUDIO_SPECTRUM_HALF_FFT];
    float twiddle_im[DAB_AUDIO_SPECTRUM_HALF_FFT];
    uint16_t bit_reverse[DAB_AUDIO_SPECTRUM_FFT_SIZE];

    if (analyzer == 0) {
        return;
    }

    memcpy(window, analyzer->window, sizeof(window));
    memcpy(twiddle_re, analyzer->twiddle_re, sizeof(twiddle_re));
    memcpy(twiddle_im, analyzer->twiddle_im, sizeof(twiddle_im));
    memcpy(bit_reverse, analyzer->bit_reverse, sizeof(bit_reverse));
    memset(analyzer, 0, sizeof(*analyzer));
    memcpy(analyzer->window, window, sizeof(window));
    memcpy(analyzer->twiddle_re, twiddle_re, sizeof(twiddle_re));
    memcpy(analyzer->twiddle_im, twiddle_im, sizeof(twiddle_im));
    memcpy(analyzer->bit_reverse, bit_reverse, sizeof(bit_reverse));
    analyzer->sample_rate = sample_rate;
    analyzer->channels = channels;
}

int dab_audio_spectrum_push_i16(dab_audio_spectrum_analyzer_t *analyzer,
                                const int16_t *pcm,
                                uint32_t frames,
                                uint8_t channels,
                                const float **display_out,
                                uint32_t *bin_count_out) {
    uint32_t frame;
    int updated = 0;

    if (display_out != 0) {
        *display_out = 0;
    }
    if (bin_count_out != 0) {
        *bin_count_out = 0u;
    }
    if (analyzer == 0 || pcm == 0 || frames == 0u || channels == 0u) {
        return 0;
    }

    if (analyzer->channels != channels) {
        dab_audio_spectrum_reset(analyzer, analyzer->sample_rate, channels);
    }

    for (frame = 0u; frame < frames; ++frame) {
        float mono = 0.0f;
        uint32_t channel;

        for (channel = 0u; channel < channels; ++channel) {
            mono += (float)pcm[(frame * channels) + channel];
        }
        mono /= (float)channels;
        analyzer->ring[analyzer->write_index] = mono;
        analyzer->write_index = (analyzer->write_index + 1u) % DAB_AUDIO_SPECTRUM_FFT_SIZE;
        if (analyzer->sample_count < DAB_AUDIO_SPECTRUM_FFT_SIZE) {
            analyzer->sample_count++;
        }
        analyzer->hop_accumulator++;

        if (analyzer->sample_count == DAB_AUDIO_SPECTRUM_FFT_SIZE &&
            analyzer->hop_accumulator >= DAB_AUDIO_SPECTRUM_HOP_SIZE) {
            analyzer->hop_accumulator -= DAB_AUDIO_SPECTRUM_HOP_SIZE;
            analyze_window(analyzer);
            updated = 1;
        }
    }

    if (updated != 0 && display_out != 0) {
        *display_out = analyzer->display;
    }
    if (updated != 0 && bin_count_out != 0) {
        *bin_count_out = DAB_AUDIO_SPECTRUM_DISPLAY_BINS;
    }
    return updated;
}
