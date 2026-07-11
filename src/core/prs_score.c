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
    uint64_t search_start;
    uint64_t search_end;
    uint64_t block_start;
    uint64_t block_end;
    uint64_t valid_start;
    uint64_t valid_end;
    uint64_t fft_start_sample;
    uint32_t guard;
    uint32_t fft;
    uint32_t guard_start_offset;
    uint32_t useful_tail_offset;
    uint32_t i;
    double corr_re = 0.0;
    double corr_im = 0.0;
    double energy_a = 0.0;
    double energy_b = 0.0;
    double best_corr_re = 0.0;
    double best_corr_im = 0.0;
    float best_score = -1.0f;

    if (score == 0 || block == 0 || candidate == 0 || result == 0) {
        return -1;
    }
    if (block->iq_interleaved == 0 || block->sample_count == 0u) {
        return -1;
    }

    *result = (dab_prs_result_t){0};

    guard = score->config.guard_samples;
    fft = score->config.fft_samples;
    search_start = dab_prs_candidate_search_start_sample(candidate);
    search_end = dab_prs_candidate_search_end_sample(candidate);
    block_start = block->sample_index;
    block_end = block_start + block->sample_count;
    if (block_end < block_start || block->sample_count < fft ||
        (block->sample_count - fft) < guard) {
        return 0;
    }

    valid_start = block_start + guard;
    valid_end = (block_end - fft) + 1u;
    if (search_start < valid_start) {
        search_start = valid_start;
    }
    if (search_end > valid_end) {
        search_end = valid_end;
    }
    if (search_start >= search_end) {
        return 0;
    }

    guard_start_offset = (uint32_t)((search_start - guard) - block_start);
    useful_tail_offset = (uint32_t)((search_start + fft - guard) - block_start);
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

    for (fft_start_sample = search_start; fft_start_sample < search_end; ++fft_start_sample) {
        if (energy_a > 0.0 && energy_b > 0.0) {
            const double score_value = ((corr_re * corr_re) + (corr_im * corr_im)) /
                                       (energy_a * energy_b);
            const float candidate_score = (float)score_value;

            if (candidate_score > best_score) {
                best_score = candidate_score;
                best_corr_re = corr_re;
                best_corr_im = corr_im;
                result->prs_peak_sample = fft_start_sample;
                result->prs_fft_start_sample = fft_start_sample;
                result->prs_symbol_start_sample = fft_start_sample - guard;
                result->prs_score = candidate_score;
                result->valid = 1;
            }
        }

        if ((fft_start_sample + 1u) < search_end) {
            const uint32_t incoming_guard_offset = guard_start_offset + guard;
            const uint32_t incoming_tail_offset = useful_tail_offset + guard;
            const double old_ai = (double)block->iq_interleaved[2u * guard_start_offset];
            const double old_aq = (double)block->iq_interleaved[(2u * guard_start_offset) + 1u];
            const double old_bi = (double)block->iq_interleaved[2u * useful_tail_offset];
            const double old_bq = (double)block->iq_interleaved[(2u * useful_tail_offset) + 1u];
            const double new_ai = (double)block->iq_interleaved[2u * incoming_guard_offset];
            const double new_aq = (double)block->iq_interleaved[(2u * incoming_guard_offset) + 1u];
            const double new_bi = (double)block->iq_interleaved[2u * incoming_tail_offset];
            const double new_bq = (double)block->iq_interleaved[(2u * incoming_tail_offset) + 1u];

            corr_re -= (old_ai * old_bi) + (old_aq * old_bq);
            corr_im -= (old_aq * old_bi) - (old_ai * old_bq);
            energy_a -= (old_ai * old_ai) + (old_aq * old_aq);
            energy_b -= (old_bi * old_bi) + (old_bq * old_bq);
            corr_re += (new_ai * new_bi) + (new_aq * new_bq);
            corr_im += (new_aq * new_bi) - (new_ai * new_bq);
            energy_a += (new_ai * new_ai) + (new_aq * new_aq);
            energy_b += (new_bi * new_bi) + (new_bq * new_bq);
            guard_start_offset++;
            useful_tail_offset++;
        }
    }

    if (!result->valid) {
        return 0;
    }

    result->prs_score = best_score;
    result->coarse_freq_error_hz = (float)(-atan2(best_corr_im, best_corr_re) *
                                           (double)score->config.sample_rate /
                                           (DAB_TWO_PI_D * (double)fft));
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

int dab_prs_estimate_snr_f32(const float *rx_i,
                             const float *rx_q,
                             const float *ref_i,
                             const float *ref_q,
                             uint32_t count,
                             dab_prs_snr_result_t *result) {
    uint32_t i;
    uint32_t used = 0u;
    double cross_re = 0.0;
    double cross_im = 0.0;
    double ref_energy = 0.0;
    double signal_energy = 0.0;
    double noise_energy = 0.0;
    double gain_re;
    double gain_im;

    if (rx_i == 0 || rx_q == 0 || ref_i == 0 || ref_q == 0 || result == 0 || count == 0u) {
        return -1;
    }

    for (i = 0u; i < count; ++i) {
        const double ri = (double)ref_i[i];
        const double rq = (double)ref_q[i];
        const double re = (double)rx_i[i];
        const double im = (double)rx_q[i];
        const double carrier_ref_energy = (ri * ri) + (rq * rq);

        if (carrier_ref_energy <= 0.0) {
            continue;
        }

        cross_re += (re * ri) + (im * rq);
        cross_im += (im * ri) - (re * rq);
        ref_energy += carrier_ref_energy;
        used++;
    }

    if (used == 0u || ref_energy <= 0.0) {
        return -1;
    }

    gain_re = cross_re / ref_energy;
    gain_im = cross_im / ref_energy;

    for (i = 0u; i < count; ++i) {
        const double ri = (double)ref_i[i];
        const double rq = (double)ref_q[i];
        const double re = (double)rx_i[i];
        const double im = (double)rx_q[i];
        const double carrier_ref_energy = (ri * ri) + (rq * rq);

        if (carrier_ref_energy <= 0.0) {
            continue;
        }

        {
            const double fit_re = (gain_re * ri) - (gain_im * rq);
            const double fit_im = (gain_re * rq) + (gain_im * ri);
            const double err_re = re - fit_re;
            const double err_im = im - fit_im;

            signal_energy += (fit_re * fit_re) + (fit_im * fit_im);
            noise_energy += (err_re * err_re) + (err_im * err_im);
        }
    }

    if (signal_energy <= 0.0 || noise_energy <= 0.0) {
        return -1;
    }

    result->signal_energy = (float)signal_energy;
    result->noise_energy = (float)noise_energy;
    result->snr_db = (float)(10.0 * log10(signal_energy / noise_energy));
    result->used_carriers = used;
    return 0;
}

int dab_prs_estimate_equalized_snr_f32(const float *rx_i,
                                       const float *rx_q,
                                       const float *ref_i,
                                       const float *ref_q,
                                       uint32_t count,
                                       dab_prs_snr_result_t *result) {
    enum { DAB_PRS_SNR_SMOOTH_RADIUS = 8 };

    uint32_t i;
    uint32_t used = 0u;
    double inv_i[1537];
    double inv_q[1537];
    uint8_t valid[1537];
    double eq_cross_re = 0.0;
    double eq_cross_im = 0.0;
    double ref_energy = 0.0;
    double signal_energy = 0.0;
    double noise_energy = 0.0;
    double fit_re;
    double fit_im;

    if (rx_i == 0 || rx_q == 0 || ref_i == 0 || ref_q == 0 || result == 0 || count == 0u || count > 1537u) {
        return -1;
    }

    for (i = 0u; i < count; ++i) {
        const double si = (double)rx_i[i];
        const double sq = (double)rx_q[i];
        const double ri = (double)ref_i[i];
        const double rq = (double)ref_q[i];
        const double sym_energy = (si * si) + (sq * sq);
        const double ref_bin_energy = (ri * ri) + (rq * rq);

        if (sym_energy <= 1.0e-12 || ref_bin_energy <= 0.0) {
            inv_i[i] = 0.0;
            inv_q[i] = 0.0;
            valid[i] = 0u;
            continue;
        }

        inv_i[i] = ((ri * si) + (rq * sq)) / sym_energy;
        inv_q[i] = ((rq * si) - (ri * sq)) / sym_energy;
        valid[i] = 1u;
    }

    for (i = 0u; i < count; ++i) {
        const double ri = (double)ref_i[i];
        const double rq = (double)ref_q[i];
        const double ref_bin_energy = (ri * ri) + (rq * rq);
        double avg_inv_re = 0.0;
        double avg_inv_im = 0.0;
        uint32_t avg_count = 0u;
        uint32_t j0;
        uint32_t j1;
        uint32_t j;
        double eq_re;
        double eq_im;

        if (ref_bin_energy <= 0.0) {
            continue;
        }

        j0 = i > DAB_PRS_SNR_SMOOTH_RADIUS ? i - DAB_PRS_SNR_SMOOTH_RADIUS : 0u;
        j1 = i + DAB_PRS_SNR_SMOOTH_RADIUS;
        if (j1 >= count) {
            j1 = count - 1u;
        }

        for (j = j0; j <= j1; ++j) {
            if (valid[j] == 0u) {
                continue;
            }
            avg_inv_re += inv_i[j];
            avg_inv_im += inv_q[j];
            avg_count++;
        }

        if (avg_count == 0u) {
            continue;
        }

        avg_inv_re /= (double)avg_count;
        avg_inv_im /= (double)avg_count;
        eq_re = ((double)rx_i[i] * avg_inv_re) - ((double)rx_q[i] * avg_inv_im);
        eq_im = ((double)rx_i[i] * avg_inv_im) + ((double)rx_q[i] * avg_inv_re);

        eq_cross_re += (eq_re * ri) + (eq_im * rq);
        eq_cross_im += (eq_im * ri) - (eq_re * rq);
        ref_energy += ref_bin_energy;
        used++;
    }

    if (used == 0u || ref_energy <= 0.0) {
        return -1;
    }

    fit_re = eq_cross_re / ref_energy;
    fit_im = eq_cross_im / ref_energy;

    for (i = 0u; i < count; ++i) {
        const double ri = (double)ref_i[i];
        const double rq = (double)ref_q[i];
        const double ref_bin_energy = (ri * ri) + (rq * rq);
        double avg_inv_re = 0.0;
        double avg_inv_im = 0.0;
        uint32_t avg_count = 0u;
        uint32_t j0;
        uint32_t j1;
        uint32_t j;
        double eq_re;
        double eq_im;
        double fit_ref_re;
        double fit_ref_im;
        double err_re;
        double err_im;

        if (ref_bin_energy <= 0.0) {
            continue;
        }

        j0 = i > DAB_PRS_SNR_SMOOTH_RADIUS ? i - DAB_PRS_SNR_SMOOTH_RADIUS : 0u;
        j1 = i + DAB_PRS_SNR_SMOOTH_RADIUS;
        if (j1 >= count) {
            j1 = count - 1u;
        }

        for (j = j0; j <= j1; ++j) {
            if (valid[j] == 0u) {
                continue;
            }
            avg_inv_re += inv_i[j];
            avg_inv_im += inv_q[j];
            avg_count++;
        }

        if (avg_count == 0u) {
            continue;
        }

        avg_inv_re /= (double)avg_count;
        avg_inv_im /= (double)avg_count;
        eq_re = ((double)rx_i[i] * avg_inv_re) - ((double)rx_q[i] * avg_inv_im);
        eq_im = ((double)rx_i[i] * avg_inv_im) + ((double)rx_q[i] * avg_inv_re);
        fit_ref_re = (fit_re * ri) - (fit_im * rq);
        fit_ref_im = (fit_re * rq) + (fit_im * ri);
        err_re = eq_re - fit_ref_re;
        err_im = eq_im - fit_ref_im;

        signal_energy += (fit_ref_re * fit_ref_re) + (fit_ref_im * fit_ref_im);
        noise_energy += (err_re * err_re) + (err_im * err_im);
    }

    if (signal_energy <= 0.0 || noise_energy <= 0.0) {
        return -1;
    }

    result->signal_energy = (float)signal_energy;
    result->noise_energy = (float)noise_energy;
    result->snr_db = (float)(10.0 * log10(signal_energy / noise_energy));
    result->used_carriers = used;
    return 0;
}

int dab_prs_score_to_snr_db(float score, float *snr_db_out) {
    const double score_d = (double)score;
    const double epsilon = 1.0e-6;
    double clamped_score;
    double snr_linear;

    if (snr_db_out == 0) {
        return -1;
    }

    if (!(score_d > 0.0) || !(score_d < 1.0)) {
        return -1;
    }

    clamped_score = score_d;
    if (clamped_score < epsilon) {
        clamped_score = epsilon;
    } else if (clamped_score > (1.0 - epsilon)) {
        clamped_score = 1.0 - epsilon;
    }

    snr_linear = clamped_score / (1.0 - clamped_score);
    *snr_db_out = (float)(10.0 * log10(snr_linear));
    return 0;
}
