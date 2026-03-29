#include "dab/core/prs_score.h"

#include <math.h>

#include "prs_template_mode_i_table.h"

#define DAB_TWO_PI_D 6.28318530717958647692

static float dab_prs_score_phase_to_timing_samples(double phase, uint32_t fft_samples) {
    if (fft_samples == 0u) {
        return 0.0f;
    }

    return (float)(phase * ((double)fft_samples / DAB_TWO_PI_D));
}

void dab_prs_score_default_config(dab_prs_score_config_t *config) {
    if (config == 0) {
        return;
    }

    config->sample_rate = 2048000u;
    config->guard_samples = 504u;
    config->fft_samples = 2048u;
}

int dab_prs_score_init(dab_prs_score_t *score, const dab_prs_score_config_t *config) {
    dab_prs_score_config_t defaults;

    if (score == 0) {
        return -1;
    }

    dab_prs_score_default_config(&defaults);
    score->config = config != 0 ? *config : defaults;
    if (score->config.sample_rate == 0u ||
        score->config.guard_samples == 0u ||
        score->config.fft_samples == 0u) {
        return -1;
    }

    return 0;
}

void dab_prs_score_reset(dab_prs_score_t *score) {
    (void)score;
}

int dab_prs_score_cp_metric_i16(const dab_prs_score_t *score,
                                const dab_sdr_block_view_t *block,
                                uint64_t fft_start_sample,
                                float *score_out,
                                float *coarse_freq_error_hz_out) {
    const uint32_t guard = score->config.guard_samples;
    const uint32_t fft = score->config.fft_samples;
    const uint64_t block_start = block->sample_index;
    const uint64_t block_end = block->sample_index + block->sample_count;
    const uint64_t guard_start_sample = fft_start_sample - guard;
    const uint64_t useful_tail_sample = fft_start_sample + fft - guard;
    uint32_t guard_start_offset;
    uint32_t useful_tail_offset;
    uint32_t i;
    double corr_re = 0.0;
    double corr_im = 0.0;
    double energy_a = 0.0;
    double energy_b = 0.0;
    double score_value;
    double phase;
    double cfo_hz;

    if (fft_start_sample < guard) {
        return -1;
    }
    if (guard_start_sample < block_start || (useful_tail_sample + guard) > block_end) {
        return -1;
    }

    guard_start_offset = (uint32_t)(guard_start_sample - block_start);
    useful_tail_offset = (uint32_t)(useful_tail_sample - block_start);

    for (i = 0u; i < guard; ++i) {
        const double ai = (double)block->iq_interleaved[2u * (guard_start_offset + i)];
        const double aq = (double)block->iq_interleaved[(2u * (guard_start_offset + i)) + 1u];
        const double bi = (double)block->iq_interleaved[2u * (useful_tail_offset + i)];
        const double bq = (double)block->iq_interleaved[(2u * (useful_tail_offset + i)) + 1u];

        corr_re += (ai * bi) + (aq * bq);
        corr_im += (aq * bi) - (ai * bq);
        energy_a += (ai * ai) + (aq * aq);
        energy_b += (bi * bi) + (bq * bq);
    }

    if (energy_a <= 0.0 || energy_b <= 0.0) {
        return -1;
    }

    score_value = ((corr_re * corr_re) + (corr_im * corr_im)) / (energy_a * energy_b);
    phase = atan2(corr_im, corr_re);
    cfo_hz = -phase * (double)score->config.sample_rate  /
                       (2.0 * 3.14159265358979323846 * (double)fft);

    *score_out = (float)score_value;
    *coarse_freq_error_hz_out = (float)cfo_hz;
    return 0;
}

int dab_prs_score_search_i16(const dab_prs_score_t *score,
                             const dab_sdr_block_view_t *block,
                             const dab_prs_candidate_t *candidate,
                             dab_prs_result_t *result) {
    const uint64_t search_start = dab_prs_candidate_search_start_sample(candidate);
    const uint64_t search_end = dab_prs_candidate_search_end_sample(candidate);
    uint64_t fft_start_sample;
    float best_score = -1.0f;
    float best_cfo = 0.0f;

    if (score == 0 || block == 0 || candidate == 0 || result == 0) {
        return -1;
    }
    if (block->iq_interleaved == 0 || block->sample_count == 0u) {
        return -1;
    }

    *result = (dab_prs_result_t){0};

    for (fft_start_sample = search_start; fft_start_sample < search_end; ++fft_start_sample) {
        float candidate_score;
        float candidate_cfo;

        if (dab_prs_score_cp_metric_i16(score,
                                        block,
                                        fft_start_sample,
                                        &candidate_score,
                                        &candidate_cfo) != 0) {
            continue;
        }

        if (candidate_score > best_score) {
            best_score = candidate_score;
            best_cfo = candidate_cfo;
            result->prs_peak_sample = fft_start_sample;
            result->prs_fft_start_sample = fft_start_sample;
            result->prs_symbol_start_sample = fft_start_sample - score->config.guard_samples;
            result->prs_score = candidate_score;
            result->coarse_freq_error_hz = candidate_cfo;
            result->valid = 1;
        }
    }

    if (!result->valid) {
        return 0;
    }

    result->prs_score = best_score;
    result->coarse_freq_error_hz = best_cfo;
    return 1;
}

int dab_prs_score_reference_f32(const float *rx_i,
                                const float *rx_q,
                                const float *ref_i,
                                const float *ref_q,
                                uint32_t count,
                                dab_prs_reference_score_t *result) {
    uint32_t i;
    double corr_re = 0.0;
    double corr_im = 0.0;
    double signal_energy = 0.0;
    double reference_energy = 0.0;
    double score_value;

    if (rx_i == 0 || rx_q == 0 || ref_i == 0 || ref_q == 0 || result == 0 || count == 0u) {
        return -1;
    }

    for (i = 0u; i < count; ++i) {
        const double si = (double)rx_i[i];
        const double sq = (double)rx_q[i];
        const double ri = (double)ref_i[i];
        const double rq = (double)ref_q[i];

        corr_re += (si * ri) + (sq * rq);
        corr_im += (sq * ri) - (si * rq);
        signal_energy += (si * si) + (sq * sq);
        reference_energy += (ri * ri) + (rq * rq);
    }

    if (signal_energy <= 0.0 || reference_energy <= 0.0) {
        return -1;
    }

    score_value = ((corr_re * corr_re) + (corr_im * corr_im)) / (signal_energy * reference_energy);

    result->score = (float)score_value;
    result->correlation_re = (float)corr_re;
    result->correlation_im = (float)corr_im;
    result->signal_energy = (float)signal_energy;
    result->reference_energy = (float)reference_energy;
    result->carrier_offset_bins = 0;
    result->fine_timing_error_samples = 0.0f;
    return 0;
}

int dab_prs_score_reference_template_f32(const float *rx_i,
                                         const float *rx_q,
                                         const dab_prs_template_t *templ,
                                         dab_prs_reference_score_t *result) {
    uint32_t i;
    double corr_re = 0.0;
    double corr_im = 0.0;
    double signal_energy = 0.0;
    double reference_energy = 0.0;
    double score_value;

    if (rx_i == 0 || rx_q == 0 || templ == 0 || result == 0 ||
        templ->ref_i == 0 || templ->ref_q == 0 || templ->active_bins == 0 ||
        templ->fft_samples == 0u) {
        return -1;
    }

    for (i = 0u; i < templ->fft_samples; ++i) {
        const double si = (double)rx_i[i];
        const double sq = (double)rx_q[i];

        if (templ->active_bins[i] == 0u) {
            continue;
        }

        {
            const double ri = (double)templ->ref_i[i];
            const double rq = (double)templ->ref_q[i];

            corr_re += (si * ri) + (sq * rq);
            corr_im += (sq * ri) - (si * rq);
            signal_energy += (si * si) + (sq * sq);
            reference_energy += (ri * ri) + (rq * rq);
        }
    }

    if (signal_energy <= 0.0 || reference_energy <= 0.0) {
        return -1;
    }

    score_value = ((corr_re * corr_re) + (corr_im * corr_im)) / (signal_energy * reference_energy);
    result->score = (float)score_value;
    result->correlation_re = (float)corr_re;
    result->correlation_im = (float)corr_im;
    result->signal_energy = (float)signal_energy;
    result->reference_energy = (float)reference_energy;
    result->carrier_offset_bins = 0;
    result->fine_timing_error_samples = 0.0f;
    return 0;
}

int dab_prs_score_reference_bins_diff_f32(const float *rx_i,
                                          const float *rx_q,
                                          const dab_prs_template_bin_t *bins,
                                          uint32_t bin_count,
                                          int32_t min_offset_bins,
                                          int32_t max_offset_bins,
                                          dab_prs_reference_score_t *result) {
    int32_t offset;
    float best_score = -1.0f;
    dab_prs_reference_score_t best = {0};

    if (rx_i == 0 || rx_q == 0 || bins == 0 || result == 0 || bin_count < 2u) {
        return -1;
    }

    for (offset = min_offset_bins; offset <= max_offset_bins; ++offset) {
        uint32_t i;
        double corr_re = 0.0;
        double corr_im = 0.0;
        double signal_energy = 0.0;
        double reference_energy = 0.0;
        uint32_t used = 0u;

        for (i = 0u; i + 1u < bin_count; ++i) {
            const int32_t b0 = (int32_t)bins[i].bin + offset;
            const int32_t b1 = (int32_t)bins[i + 1u].bin + offset;
            double s0_re;
            double s0_im;
            double s1_re;
            double s1_im;
            double p0_re;
            double p0_im;
            double p1_re;
            double p1_im;
            double sd_re;
            double sd_im;
            double pd_re;
            double pd_im;

            if (b0 < 0 || b1 < 0 || b0 >= 2048 || b1 >= 2048) {
                continue;
            }

            s0_re = (double)rx_i[b0];
            s0_im = (double)rx_q[b0];
            s1_re = (double)rx_i[b1];
            s1_im = (double)rx_q[b1];
            p0_re = (double)bins[i].re;
            p0_im = (double)bins[i].im;
            p1_re = (double)bins[i + 1u].re;
            p1_im = (double)bins[i + 1u].im;
            sd_re = (s0_re * s1_re) + (s0_im * s1_im);
            sd_im = (s0_im * s1_re) - (s0_re * s1_im);
            pd_re = (p0_re * p1_re) + (p0_im * p1_im);
            pd_im = (p0_im * p1_re) - (p0_re * p1_im);

            corr_re += (sd_re * pd_re) + (sd_im * pd_im);
            corr_im += (sd_im * pd_re) - (sd_re * pd_im);
            signal_energy += (sd_re * sd_re) + (sd_im * sd_im);
            reference_energy += (pd_re * pd_re) + (pd_im * pd_im);
            used++;
        }

        if (used == 0u || signal_energy <= 0.0 || reference_energy <= 0.0) {
            continue;
        }

        {
            const double score = ((corr_re * corr_re) + (corr_im * corr_im)) / (signal_energy * reference_energy);
            if ((float)score > best_score) {
                best_score = (float)score;
                best.score = (float)score;
                best.correlation_re = (float)corr_re;
                best.correlation_im = (float)corr_im;
                best.signal_energy = (float)signal_energy;
                best.reference_energy = (float)reference_energy;
                best.carrier_offset_bins = offset;
                best.fine_timing_error_samples =
                    dab_prs_score_phase_to_timing_samples(atan2(corr_im, corr_re), 2048u);
            }
        }
    }

    if (best_score < 0.0f) {
        return -1;
    }

    *result = best;
    return 0;
}

int dab_prs_score_reference_mode_i_diff_f32(const float *rx_i,
                                            const float *rx_q,
                                            dab_prs_reference_score_t *result) {
    return dab_prs_score_reference_bins_diff_f32(rx_i,
                                                 rx_q,
                                                 DAB_PRS_MODE_I_BINS,
                                                 DAB_PRS_MODE_I_BIN_COUNT,
                                                 -4,
                                                 4,
                                                 result);
}

int dab_prs_score_symbol_i16(const dab_prs_fft_t *fft,
                             const dab_sdr_block_view_t *block,
                             uint64_t fft_start_sample,
                             const dab_prs_template_t *templ,
                             float *fft_i,
                             float *fft_q,
                             dab_prs_reference_score_t *result) {
    if (fft == 0 || block == 0 || templ == 0 || fft_i == 0 || fft_q == 0 || result == 0) {
        return -1;
    }
    if (templ->fft_samples != fft->config.fft_samples) {
        return -1;
    }
    if (dab_prs_fft_execute_i16(fft, block, fft_start_sample, fft_i, fft_q) != 0) {
        return -1;
    }
    return dab_prs_score_reference_template_f32(fft_i, fft_q, templ, result);
}

int dab_prs_score_symbol_i16_mode_i_diff(const dab_prs_fft_t *fft,
                                         const dab_sdr_block_view_t *block,
                                         uint64_t fft_start_sample,
                                         float *fft_i,
                                         float *fft_q,
                                         dab_prs_reference_score_t *result) {
    return dab_prs_score_symbol_i16_mode_i_diff_cfo(fft,
                                                    block,
                                                    fft_start_sample,
                                                    0.0f,
                                                    fft_i,
                                                    fft_q,
                                                    result);
}

int dab_prs_score_symbol_i16_mode_i_diff_cfo(const dab_prs_fft_t *fft,
                                             const dab_sdr_block_view_t *block,
                                             uint64_t fft_start_sample,
                                             float cfo_hz,
                                             float *fft_i,
                                             float *fft_q,
                                             dab_prs_reference_score_t *result) {
    if (fft == 0 || block == 0 || fft_i == 0 || fft_q == 0 || result == 0) {
        return -1;
    }
    if (dab_prs_fft_execute_i16_cfo(fft, block, fft_start_sample, cfo_hz, fft_i, fft_q) != 0) {
        return -1;
    }
    return dab_prs_score_reference_mode_i_diff_f32(fft_i, fft_q, result);
}
