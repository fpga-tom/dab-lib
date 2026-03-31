#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "audio_playback.h"
#include "dab/config.h"
#include "dab/core/fic.h"
#include "dab/core/null_detect.h"
#include "dab/core/ofdm.h"
#include "dab/core/prs_acquire.h"
#include "dab/core/prs_fft.h"
#include "dab/core/prs_score.h"
#include "dab/core/sync.h"
#include "dab/core/tframe.h"
#include "dab/core/msc.h"
#include "dab/hal/sdr_device.h"
#include "dab/hal/sdrplay_device.h"
#include "dab/simd/simd.h"

typedef struct {
    int16_t *iq;
    uint32_t capacity_samples;
    uint32_t sample_count;
    uint64_t start_sample_index;
} history_buffer_t;

typedef struct {
    int pending;
    int predicted;
    dab_null_event_t event;
    dab_prs_candidate_t candidate;
} pending_candidate_t;

typedef struct {
    char magic[8];
    uint32_t symbol_count;
    int8_t softbits[DAB_TFRAME_FIC_SYMBOLS][2][1536];
} fic_symbol_dump_t;

typedef struct {
    char magic[8];
    uint32_t symbol_count;
    float carriers[DAB_TFRAME_FIC_SYMBOLS][2][1536];
} fic_carrier_dump_t;

typedef struct {
    char magic[8];
    uint32_t symbol_count;
    float carriers[DAB_TFRAME_FIC_SYMBOLS][2][1537];
} fic_diff_dump_t;

typedef struct {
    char magic[8];
    uint32_t symbol_count;
    float carriers[DAB_TFRAME_FIC_SYMBOLS + 1u][2][1537];
} fic_active_dump_t;

typedef struct {
    char magic[8];
    uint32_t symbol_count;
    float carriers[DAB_TFRAME_FIC_SYMBOLS + 1u][2][1537];
} fic_raw_active_dump_t;

typedef struct {
    char magic[8];
    uint32_t fft_samples;
    float bins[2][2048];
} prs_fft_dump_t;

static volatile sig_atomic_t g_stop_requested = 0;
static int g_debug_logs = 1;

static void on_signal_stop(int signum) {
    (void)signum;
    g_stop_requested = 1;
}

static void print_usage(const char *argv0) {
    fprintf(stderr,
            "usage: %s --freq <hz> [--agc on|off] [--sid <sid>] [--debug on|off]\n"
            "       %s -f <hz> [-a on|off] [-s <sid>] [-d on|off]\n",
            argv0 != 0 ? argv0 : "dab_sdrplay_audio",
            argv0 != 0 ? argv0 : "dab_sdrplay_audio");
}

static int parse_u64_arg(const char *value, uint64_t *out) {
    char *end = 0;
    unsigned long long parsed;

    if (value == 0 || out == 0) {
        return -1;
    }
    parsed = strtoull(value, &end, 10);
    if (end == value || (end != 0 && *end != '\0')) {
        return -1;
    }
    *out = (uint64_t)parsed;
    return 0;
}

static int parse_agc_arg(const char *value, dab_sdrplay_gain_mode_t *mode) {
    if (value == 0 || mode == 0) {
        return -1;
    }
    if (strcmp(value, "on") == 0 || strcmp(value, "auto") == 0 || strcmp(value, "1") == 0) {
        *mode = DAB_SDRPLAY_GAIN_MODE_AUTO;
        return 0;
    }
    if (strcmp(value, "off") == 0 || strcmp(value, "manual") == 0 || strcmp(value, "0") == 0) {
        *mode = DAB_SDRPLAY_GAIN_MODE_MANUAL;
        return 0;
    }
    return -1;
}

static int parse_u32_base0_arg(const char *value, uint32_t *out) {
    char *end = 0;
    unsigned long parsed;

    if (value == 0 || out == 0) {
        return -1;
    }
    parsed = strtoul(value, &end, 0);
    if (end == value || (end != 0 && *end != '\0')) {
        return -1;
    }
    *out = (uint32_t)parsed;
    return 0;
}

static int parse_on_off_arg(const char *value, int *enabled) {
    if (value == 0 || enabled == 0) {
        return -1;
    }
    if (strcmp(value, "on") == 0 || strcmp(value, "1") == 0 || strcmp(value, "true") == 0) {
        *enabled = 1;
        return 0;
    }
    if (strcmp(value, "off") == 0 || strcmp(value, "0") == 0 || strcmp(value, "false") == 0) {
        *enabled = 0;
        return 0;
    }
    return -1;
}

#define DAB_DEBUG_LOG(...)              \
    do {                                \
        if (g_debug_logs != 0) {        \
            printf(__VA_ARGS__);        \
        }                               \
    } while (0)

static const dab_msc_decoded_component_t *find_selected_audio_component(const dab_msc_state_t *msc,
                                                                        uint32_t sid) {
    const dab_msc_decoded_component_t *component;
    uint32_t i;

    if (msc == 0) {
        return 0;
    }

    component = dab_msc_find_primary_decoded_component(msc, sid);
    if (component != 0 && dab_msc_component_kind(component->tmid) == DAB_MSC_COMPONENT_STREAM_AUDIO) {
        return component;
    }

    for (i = 0u; i < msc->decoded_component_count; ++i) {
        const dab_msc_decoded_component_t *candidate = &msc->decoded_components[i];

        if (candidate->used != 0u &&
            candidate->sid == sid &&
            dab_msc_component_kind(candidate->tmid) == DAB_MSC_COMPONENT_STREAM_AUDIO) {
            return candidate;
        }
    }

    return 0;
}

static const dab_msc_decoded_component_t *find_first_audio_component(const dab_msc_state_t *msc) {
    uint32_t i;

    if (msc == 0) {
        return 0;
    }

    for (i = 0u; i < msc->decoded_component_count; ++i) {
        const dab_msc_decoded_component_t *candidate = &msc->decoded_components[i];

        if (candidate->used != 0u &&
            candidate->primary != 0u &&
            dab_msc_component_kind(candidate->tmid) == DAB_MSC_COMPONENT_STREAM_AUDIO) {
            return candidate;
        }
    }

    for (i = 0u; i < msc->decoded_component_count; ++i) {
        const dab_msc_decoded_component_t *candidate = &msc->decoded_components[i];

        if (candidate->used != 0u &&
            dab_msc_component_kind(candidate->tmid) == DAB_MSC_COMPONENT_STREAM_AUDIO) {
            return candidate;
        }
    }

    return 0;
}

static void reset_runtime_state(dab_null_detect_t *detector,
                                dab_sync_t *sync,
                                dab_ofdm_t *ofdm,
                                dab_tframe_t *tframe,
                                dab_fic_state_t *fic,
                                dab_msc_state_t *msc,
                                history_buffer_t *history,
                                pending_candidate_t *pending,
                                uint64_t *last_predicted_fft_start,
                                dab_audio_playback_t *audio,
                                int audio_debug_enabled,
                                uint32_t *announced_audio_sid) {
    if (detector != 0) {
        dab_null_detect_reset(detector);
    }
    if (sync != 0) {
        dab_sync_reset(sync);
    }
    if (ofdm != 0) {
        dab_ofdm_reset(ofdm);
    }
    if (tframe != 0) {
        dab_tframe_reset(tframe);
    }
    if (fic != 0) {
        dab_fic_reset(fic);
    }
    if (msc != 0) {
        dab_msc_reset(msc);
    }
    if (history != 0) {
        history->sample_count = 0u;
    }
    if (pending != 0) {
        *pending = (pending_candidate_t){0};
    }
    if (last_predicted_fft_start != 0) {
        *last_predicted_fft_start = 0u;
    }
    if (announced_audio_sid != 0) {
        *announced_audio_sid = 0u;
    }
#if defined(DAB_HAVE_AUDIO_PLAYBACK)
    if (audio != 0) {
        dab_audio_playback_close(audio);
        audio->debug_enabled = (uint8_t)(audio_debug_enabled != 0);
    }
#else
    (void)audio;
    (void)audio_debug_enabled;
#endif
}

static uint32_t fic_repo_valid_subchannel_count(const dab_fic_database_t *db) {
    uint32_t i;
    uint32_t count = 0u;

    if (db == 0) {
        return 0u;
    }
    for (i = 0u; i < DAB_FIC_MAX_SUBCHANNELS; ++i) {
        if (db->subchannels[i].valid != 0u) {
            count++;
        }
    }
    return count;
}

static const dab_fic_label_t *fic_repo_find_label(const dab_fic_database_t *db,
                                                  dab_fic_label_scope_t scope,
                                                  uint8_t is_programme,
                                                  uint32_t sid,
                                                  uint16_t eid,
                                                  uint8_t scids) {
    uint16_t i;

    if (db == 0) {
        return 0;
    }
    for (i = 0u; i < db->label_count; ++i) {
        const dab_fic_label_t *label = &db->labels[i];

        if (label->used == 0u || label->scope != scope) {
            continue;
        }
        if (scope == DAB_FIC_LABEL_SCOPE_ENSEMBLE && label->eid == eid) {
            return label;
        }
        if (scope == DAB_FIC_LABEL_SCOPE_SERVICE &&
            label->is_programme == is_programme &&
            label->sid == sid) {
            return label;
        }
        if (scope == DAB_FIC_LABEL_SCOPE_COMPONENT &&
            label->is_programme == is_programme &&
            label->sid == sid &&
            label->scids == scids) {
            return label;
        }
    }
    return 0;
}

static void print_fic_repo_summary(const dab_fic_repo_t *repo) {
    const dab_fic_database_t *db;
    const dab_fic_label_t *ensemble_label;
    uint32_t subchannel_count;
    uint32_t subchid;
    uint16_t service_index;

    if (repo == 0) {
        return;
    }
    db = &repo->current;
    subchannel_count = fic_repo_valid_subchannel_count(db);
    ensemble_label = fic_repo_find_label(db,
                                         DAB_FIC_LABEL_SCOPE_ENSEMBLE,
                                         0u,
                                         0u,
                                         db->eid,
                                         0u);

    printf("fic_repo eid=0x%04x subchannels=%u services=%u labels=%u",
           db->eid,
           subchannel_count,
           db->service_count,
           db->label_count);
    if (ensemble_label != 0 && ensemble_label->text[0] != '\0') {
        printf(" ensemble_label=\"%s\"", ensemble_label->text);
    }
    printf("\n");

    for (subchid = 0u; subchid < DAB_FIC_MAX_SUBCHANNELS; ++subchid) {
        const dab_fic_subchannel_t *subchannel = &db->subchannels[subchid];

        if (subchannel->valid == 0u) {
            continue;
        }
        if (subchannel->protection_kind == DAB_FIC_PROTECTION_UEP) {
            printf(" fic_subch id=%u start_cu=%u kind=UEP table=%u index=%u\n",
                   subchannel->subchid,
                   subchannel->start_cu,
                   subchannel->table_switch,
                   subchannel->protection_index);
        } else {
            printf(" fic_subch id=%u start_cu=%u kind=EEP option=%u level=%u size_cu=%u\n",
                   subchannel->subchid,
                   subchannel->start_cu,
                   subchannel->protection_option,
                   subchannel->protection_level,
                   subchannel->size_cu);
        }
    }

    for (service_index = 0u; service_index < db->service_count; ++service_index) {
        const dab_fic_service_t *service = &db->services[service_index];
        const dab_fic_label_t *service_label =
            fic_repo_find_label(db,
                                DAB_FIC_LABEL_SCOPE_SERVICE,
                                service->is_programme,
                                service->sid,
                                0u,
                                0u);

        if (service->component_count > 0u) {
            const dab_fic_service_component_t *component = &service->components[0];
            printf(" fic_service sid=0x%0*x programme=%u caid=%u comps=%u label=\"%s\" first_comp=(tmid=%u type=%u",
                   service->is_programme != 0u ? 4 : 8,
                   service->sid,
                   service->is_programme,
                   service->caid,
                   service->component_count,
                   service_label != 0 ? service_label->text : "",
                   component->tmid,
                   component->sc_type);
            if (component->has_subchid != 0u) {
                printf(" subch=%u", component->subchid);
            }
            printf(")\n");
        } else {
            printf(" fic_service sid=0x%0*x programme=%u caid=%u comps=0 label=\"%s\"\n",
                   service->is_programme != 0u ? 4 : 8,
                   service->sid,
                   service->is_programme,
                   service->caid,
                   service_label != 0 ? service_label->text : "");
        }
    }
}

static uint64_t apply_prs_fine_timing(uint64_t fft_start_sample,
                                      float fine_timing_error_samples,
                                      float max_abs_timing_error_samples,
                                      float *applied_timing_error_samples) {
    const float abs_timing = fabsf(fine_timing_error_samples);

    if (applied_timing_error_samples != 0) {
        *applied_timing_error_samples = 0.0f;
    }
    if (!(abs_timing > 0.0f) || abs_timing > max_abs_timing_error_samples) {
        return fft_start_sample;
    }

    {
        const int32_t correction = (int32_t)lroundf(fine_timing_error_samples);

        if (correction == 0) {
            return fft_start_sample;
        }
        if (applied_timing_error_samples != 0) {
            *applied_timing_error_samples = (float)correction;
        }
        if (correction < 0) {
            return fft_start_sample + (uint64_t)(-correction);
        }
        if (fft_start_sample < (uint64_t)correction) {
            *applied_timing_error_samples = 0.0f;
            return fft_start_sample;
        }
        return fft_start_sample - (uint64_t)correction;
    }
}

static int append_fft_gnuplot_frame(FILE *fp,
                                    uint32_t frame_index,
                                    const char *kind,
                                    uint64_t fft_start_sample,
                                    const float *carrier_i,
                                    const float *carrier_q,
                                    uint32_t carrier_count) {
    uint32_t carrier;

    if (fp == 0 || carrier_i == 0 || carrier_q == 0 || carrier_count == 0u) {
        return -1;
    }

    if (fprintf(fp,
                "# frame=%u kind=%s fft_start_sample=%llu\n",
                frame_index,
                kind != 0 ? kind : "unknown",
                (unsigned long long)fft_start_sample) < 0) {
        return -1;
    }
    for (carrier = 0u; carrier < carrier_count; ++carrier) {
        const float re = carrier_i[carrier];
        const float im = carrier_q[carrier];
        const float mag = sqrtf((re * re) + (im * im));

        if (fprintf(fp, "%u %.9g %.9g %.9g\n", carrier, re, im, mag) < 0) {
            return -1;
        }
    }
    if (fputs("\n\n", fp) == EOF) {
        return -1;
    }
    return fflush(fp) == 0 ? 0 : -1;
}

static void phase_correct_prs_for_plot(const dab_ofdm_t *ofdm,
                                       const float *carrier_i,
                                       const float *carrier_q,
                                       float *out_i,
                                       float *out_q) {
    double corr_re = 0.0;
    double corr_im = 0.0;
    float rot_re;
    float rot_im;
    uint32_t carrier;

    if (ofdm == 0 || carrier_i == 0 || carrier_q == 0 || out_i == 0 || out_q == 0) {
        return;
    }

    for (carrier = 0u; carrier < DAB_MODE_I_ACTIVE_CARRIERS; ++carrier) {
        if (carrier == DAB_MODE_I_ACTIVE_CARRIER_HALF) {
            continue;
        }
        corr_re += ((double)carrier_i[carrier] * (double)ofdm->prs_ref_i[carrier]) +
                   ((double)carrier_q[carrier] * (double)ofdm->prs_ref_q[carrier]);
        corr_im += ((double)carrier_q[carrier] * (double)ofdm->prs_ref_i[carrier]) -
                   ((double)carrier_i[carrier] * (double)ofdm->prs_ref_q[carrier]);
    }

    if (corr_re == 0.0 && corr_im == 0.0) {
        memcpy(out_i, carrier_i, sizeof(*out_i) * DAB_MODE_I_ACTIVE_CARRIERS);
        memcpy(out_q, carrier_q, sizeof(*out_q) * DAB_MODE_I_ACTIVE_CARRIERS);
        return;
    }

    {
        const float phase = atan2f((float)corr_im, (float)corr_re);
        rot_re = cosf(-phase);
        rot_im = sinf(-phase);
    }

    for (carrier = 0u; carrier < DAB_MODE_I_ACTIVE_CARRIERS; ++carrier) {
        const float re = carrier_i[carrier];
        const float im = carrier_q[carrier];

        out_i[carrier] = (re * rot_re) - (im * rot_im);
        out_q[carrier] = (re * rot_im) + (im * rot_re);
    }
    out_i[DAB_MODE_I_ACTIVE_CARRIER_HALF] = 0.0f;
    out_q[DAB_MODE_I_ACTIVE_CARRIER_HALF] = 0.0f;
}

static float decision_directed_phase_error_deg(const float *re, const float *im, uint32_t count) {
    double corr_re = 0.0;
    double corr_im = 0.0;
    uint32_t i;

    if (re == 0 || im == 0 || count == 0u) {
        return 0.0f;
    }

    for (i = 0u; i < count; ++i) {
        const float dec_re = re[i] >= 0.0f ? 1.0f : -1.0f;
        const float dec_im = im[i] >= 0.0f ? 1.0f : -1.0f;

        corr_re += ((double)re[i] * (double)dec_re) + ((double)im[i] * (double)dec_im);
        corr_im += ((double)im[i] * (double)dec_re) - ((double)re[i] * (double)dec_im);
    }

    if (corr_re == 0.0 && corr_im == 0.0) {
        return 0.0f;
    }

    return (float)(atan2(corr_im, corr_re) * (180.0 / 3.14159265358979323846));
}

static void rotate_complex_f32(float *re, float *im, uint32_t count, float phase_deg) {
    const float phase_rad = phase_deg * (float)(3.14159265358979323846 / 180.0);
    const float rot_re = cosf(phase_rad);
    const float rot_im = sinf(phase_rad);
    uint32_t i;

    if (re == 0 || im == 0) {
        return;
    }

    for (i = 0u; i < count; ++i) {
        const float in_re = re[i];
        const float in_im = im[i];
        re[i] = (in_re * rot_re) - (in_im * rot_im);
        im[i] = (in_re * rot_im) + (in_im * rot_re);
    }
}

static void init_predicted_candidate(const dab_prs_acquire_t *acquire,
                                     const dab_sync_t *sync,
                                     dab_prs_candidate_t *candidate) {
    uint64_t prs_fft_start_sample;

    if (acquire == 0 || sync == 0 || candidate == 0) {
        return;
    }

    prs_fft_start_sample = dab_sync_predict_next_frame_start(sync);
    candidate->prs_fft_start_sample = prs_fft_start_sample;
    candidate->prs_symbol_start_sample = prs_fft_start_sample - acquire->config.guard_samples;
    candidate->null_start_sample = candidate->prs_symbol_start_sample - acquire->config.null_samples;
    candidate->timing_search_start_offset = sync->config.track_timing_window_samples / 2u;
    candidate->timing_search_length = sync->config.track_timing_window_samples;
}

static unsigned int env_u32(const char *name, unsigned int fallback) {
    const char *value = getenv(name);
    char *end = 0;
    unsigned long parsed;

    if (value == 0 || value[0] == '\0') {
        return fallback;
    }
    parsed = strtoul(value, &end, 10);
    if (end == value || *end != '\0') {
        return fallback;
    }
    return (unsigned int)parsed;
}

static int env_i32(const char *name, int fallback) {
    const char *value = getenv(name);
    char *end = 0;
    long parsed;

    if (value == 0 || value[0] == '\0') {
        return fallback;
    }
    parsed = strtol(value, &end, 10);
    if (end == value || *end != '\0') {
        return fallback;
    }
    return (int)parsed;
}

static unsigned long long env_u64(const char *name, unsigned long long fallback) {
    const char *value = getenv(name);
    char *end = 0;
    unsigned long long parsed;

    if (value == 0 || value[0] == '\0') {
        return fallback;
    }
    parsed = strtoull(value, &end, 10);
    if (end == value || *end != '\0') {
        return fallback;
    }
    return parsed;
}

static float env_f32(const char *name, float fallback) {
    const char *value = getenv(name);
    char *end = 0;
    float parsed;

    if (value == 0 || value[0] == '\0') {
        return fallback;
    }
    parsed = strtof(value, &end);
    if (end == value || *end != '\0') {
        return fallback;
    }
    return parsed;
}

static dab_sdrplay_gain_mode_t env_gain_mode(const char *name, dab_sdrplay_gain_mode_t fallback) {
    const char *value = getenv(name);

    if (value == 0 || value[0] == '\0') {
        return fallback;
    }
    if (strcmp(value, "auto") == 0) {
        return DAB_SDRPLAY_GAIN_MODE_AUTO;
    }
    if (strcmp(value, "manual") == 0) {
        return DAB_SDRPLAY_GAIN_MODE_MANUAL;
    }
    return fallback;
}

static int write_binary_file(const char *path, const void *data, size_t size) {
    FILE *fp;

    if (path == 0 || path[0] == '\0' || data == 0) {
        return -1;
    }

    fp = fopen(path, "wb");
    if (fp == 0) {
        return -1;
    }
    if (fwrite(data, 1u, size, fp) != size) {
        fclose(fp);
        return -1;
    }
    fclose(fp);
    return 0;
}

static int append_ref_samples_file(FILE *fp,
                                   float **scratch_iq,
                                   uint32_t *scratch_capacity_samples,
                                   const dab_sdr_block_view_t *block) {
    uint32_t i;

    if (fp == 0 || scratch_iq == 0 || scratch_capacity_samples == 0 || block == 0 ||
        block->iq_interleaved == 0) {
        return -1;
    }

    if (*scratch_capacity_samples < block->sample_count) {
        float *next = (float *)realloc(*scratch_iq, sizeof(float) * 2u * block->sample_count);
        if (next == 0) {
            return -1;
        }
        *scratch_iq = next;
        *scratch_capacity_samples = block->sample_count;
    }

    for (i = 0u; i < block->sample_count; ++i) {
        (*scratch_iq)[2u * i] = (float)block->iq_interleaved[2u * i];
        (*scratch_iq)[2u * i + 1u] = (float)block->iq_interleaved[2u * i + 1u];
    }

    return fwrite(*scratch_iq, sizeof(float) * 2u, block->sample_count, fp) == block->sample_count ? 0 : -1;
}

static int history_init(history_buffer_t *history, uint32_t capacity_samples) {
    if (history == 0 || capacity_samples == 0u) {
        return -1;
    }
    history->iq = (int16_t *)calloc((size_t)capacity_samples * 2u, sizeof(*history->iq));
    if (history->iq == 0) {
        return -1;
    }
    history->capacity_samples = capacity_samples;
    history->sample_count = 0u;
    history->start_sample_index = 0u;
    return 0;
}

static void history_destroy(history_buffer_t *history) {
    if (history == 0) {
        return;
    }
    free(history->iq);
    history->iq = 0;
    history->capacity_samples = 0u;
    history->sample_count = 0u;
    history->start_sample_index = 0u;
}

static void history_reset(history_buffer_t *history, const dab_sdr_block_view_t *block) {
    uint32_t keep;

    if (history == 0 || block == 0 || history->iq == 0 || block->iq_interleaved == 0) {
        return;
    }

    keep = block->sample_count > history->capacity_samples ? history->capacity_samples : block->sample_count;
    memcpy(history->iq,
           block->iq_interleaved + (2u * (block->sample_count - keep)),
           sizeof(*history->iq) * 2u * keep);
    history->sample_count = keep;
    history->start_sample_index = block->sample_index + (block->sample_count - keep);
}

static void history_append(history_buffer_t *history, const dab_sdr_block_view_t *block) {
    uint32_t drop;
    uint32_t keep;
    uint64_t expected_next;

    if (history == 0 || block == 0 || history->iq == 0 || block->iq_interleaved == 0) {
        return;
    }
    if (history->sample_count == 0u) {
        history_reset(history, block);
        return;
    }

    expected_next = history->start_sample_index + history->sample_count;
    if (block->sample_index != expected_next) {
        history_reset(history, block);
        return;
    }

    keep = block->sample_count;
    if (keep > history->capacity_samples) {
        history_reset(history, block);
        return;
    }

    if ((history->sample_count + keep) > history->capacity_samples) {
        drop = (history->sample_count + keep) - history->capacity_samples;
        memmove(history->iq,
                history->iq + (2u * drop),
                sizeof(*history->iq) * 2u * (history->sample_count - drop));
        history->sample_count -= drop;
        history->start_sample_index += drop;
    }

    memcpy(history->iq + (2u * history->sample_count),
           block->iq_interleaved,
           sizeof(*history->iq) * 2u * keep);
    history->sample_count += keep;
}

static dab_sdr_block_view_t history_view(const history_buffer_t *history) {
    dab_sdr_block_view_t view = {0};

    if (history == 0) {
        return view;
    }
    view.iq_interleaved = history->iq;
    view.sample_count = history->sample_count;
    view.sample_index = history->start_sample_index;
    view.timestamp_ns = 0u;
    return view;
}

static int history_contains_sample_range(const history_buffer_t *history,
                                         uint64_t start_sample,
                                         uint64_t end_sample) {
    uint64_t history_end;

    if (history == 0 || history->sample_count == 0u) {
        return 0;
    }
    history_end = history->start_sample_index + history->sample_count;
    return start_sample >= history->start_sample_index && end_sample <= history_end;
}

int main(int argc, char **argv) {
    dab_sdr_open_params_t params = {"sdrplay", NULL, NULL};
    dab_sdr_device_t *device = 0;
    dab_sdr_stream_t *stream = 0;
    dab_stream_config_t stream_config;
    dab_sdrplay_config_t device_config;
    dab_null_detect_t detector;
    dab_null_detect_config_t detector_config;
    dab_prs_acquire_t acquire;
    dab_prs_acquire_config_t acquire_config;
    dab_prs_score_t cp_score;
    dab_prs_score_config_t score_config;
    dab_prs_fft_t fft;
    dab_prs_fft_config_t fft_config;
    dab_ofdm_t ofdm;
    dab_ofdm_config_t ofdm_config;
    dab_sync_t sync;
    dab_sync_config_t sync_config;
    dab_tframe_t tframe;
    dab_tframe_buffers_t tframe_buffers;
    dab_fic_state_t fic;
    dab_msc_state_t msc;
    history_buffer_t history = {0};
    pending_candidate_t pending = {0};
    dab_sdr_block_view_t src_block;
    dab_null_event_t event;
    const dab_simd_ops_t *simd_ops = dab_simd_get_ops();
    float *fft_i = 0;
    float *fft_q = 0;
    float *active_i = 0;
    float *active_q = 0;
    float *raw_active_i = 0;
    float *raw_active_q = 0;
    float *prev_active_i = 0;
    float *prev_active_q = 0;
    float *diff_i = 0;
    float *diff_q = 0;
    float *deintl_i = 0;
    float *deintl_q = 0;
    int8_t *soft_a = 0;
    int8_t *soft_b = 0;
    int8_t *fic_softbits = 0;
    int8_t *msc_softbits = 0;
    int8_t *frame_softbits = 0;
    uint64_t blocks_read = 0u;
    unsigned int dropped_blocks = 0u;
    uint64_t last_predicted_fft_start = 0u;
    float ref_threshold = env_f32("DAB_SYNC_PROBE_REF_SCORE_THRESHOLD", 0.10f);
    float prs_fine_timing_limit = env_f32("DAB_SYNC_PROBE_PRS_FINE_TIMING_LIMIT", 32.0f);
    int apply_data_cpe_correction = env_i32("DAB_SYNC_PROBE_APPLY_DATA_CPE_CORRECTION", 0);
    int audio_debug_enabled = env_i32("DAB_SDRPLAY_AUDIO_DEBUG", 0);
    int debug_logs_enabled = env_i32("DAB_SDRPLAY_AUDIO_DEBUG_LOGS", 1);
    const char *dump_ref_samples_path = getenv("DAB_SYNC_PROBE_DUMP_REF_SAMPLES_PATH");
    const char *dump_fic_path = getenv("DAB_SYNC_PROBE_DUMP_FIC_PATH");
    const char *dump_fic_symbols_path = getenv("DAB_SYNC_PROBE_DUMP_FIC_SYMBOLS_PATH");
    const char *dump_fic_carriers_path = getenv("DAB_SYNC_PROBE_DUMP_FIC_CARRIERS_PATH");
    const char *dump_fic_diff_path = getenv("DAB_SYNC_PROBE_DUMP_FIC_DIFF_PATH");
    const char *dump_fic_active_path = getenv("DAB_SYNC_PROBE_DUMP_FIC_ACTIVE_PATH");
    const char *dump_fic_raw_active_path = getenv("DAB_SYNC_PROBE_DUMP_FIC_RAW_ACTIVE_PATH");
    const char *dump_prs_fft_path = getenv("DAB_SYNC_PROBE_DUMP_PRS_FFT_PATH");
    const char *dump_prs_fft_gnuplot_path = getenv("DAB_SYNC_PROBE_DUMP_PRS_FFT_GNUPLOT_PATH");
    int fic_dump_written = 0;
    int fic_symbols_dump_written = 0;
    int fic_carriers_dump_written = 0;
    int fic_diff_dump_written = 0;
    int fic_active_dump_written = 0;
    int fic_raw_active_dump_written = 0;
    int prs_fft_dump_written = 0;
    fic_symbol_dump_t fic_symbol_dump = { { 'D', 'A', 'B', 'F', 'I', 'C', '1', '\0' }, DAB_TFRAME_FIC_SYMBOLS, {{{0}}} };
    fic_carrier_dump_t fic_carrier_dump = { { 'D', 'A', 'B', 'F', 'I', 'C', '2', '\0' }, DAB_TFRAME_FIC_SYMBOLS, {{{0}}} };
    fic_diff_dump_t fic_diff_dump = { { 'D', 'A', 'B', 'F', 'I', 'C', '3', '\0' }, DAB_TFRAME_FIC_SYMBOLS, {{{0}}} };
    fic_active_dump_t fic_active_dump = { { 'D', 'A', 'B', 'F', 'I', 'C', '4', '\0' }, DAB_TFRAME_FIC_SYMBOLS + 1u, {{{0}}} };
    fic_raw_active_dump_t fic_raw_active_dump = { { 'D', 'A', 'B', 'F', 'I', 'C', '5', '\0' }, DAB_TFRAME_FIC_SYMBOLS + 1u, {{{0}}} };
    prs_fft_dump_t prs_fft_dump = { { 'D', 'A', 'B', 'P', 'R', 'S', 'F', '\0' }, 2048u, {{0}} };
    FILE *ref_samples_fp = 0;
    FILE *prs_fft_gnuplot_fp = 0;
    float *ref_samples_scratch = 0;
    uint32_t ref_samples_scratch_capacity = 0u;
    uint32_t prs_fft_gnuplot_frame = 0u;
    uint32_t selected_audio_sid = 0u;
    uint32_t announced_audio_sid = 0u;
    uint8_t selected_audio_sid_valid = 0u;
    uint8_t freq_set = 0u;
    uint8_t prev_locked = 0u;
    uint8_t reacquire_report_pending = 0u;
    uint8_t playing_reported = 0u;
    uint8_t waiting_for_audio_reported = 0u;
    uint64_t audio_timeout_samples = 0u;
    uint64_t samples_since_audio = 0u;
    int rc = 1;
#if defined(DAB_HAVE_AUDIO_PLAYBACK)
    dab_audio_playback_t audio = {0};
    audio.debug_enabled = (uint8_t)(audio_debug_enabled != 0);
#endif

    if (signal(SIGINT, on_signal_stop) == SIG_ERR || signal(SIGTERM, on_signal_stop) == SIG_ERR) {
        fprintf(stderr, "signal handler install failed\n");
        return 2;
    }

    dab_sdrplay_default_config(&device_config);
    device_config.gain_mode = env_gain_mode("DAB_SYNC_PROBE_GAIN_MODE", DAB_SDRPLAY_GAIN_MODE_MANUAL);
    device_config.if_mode = DAB_SDRPLAY_IF_MODE_ZERO_IF;
    device_config.antenna = DAB_SDRPLAY_ANTENNA_A;
    device_config.lna_state = (unsigned char)env_u32("DAB_SYNC_PROBE_LNA_STATE", 0u);
    device_config.if_gain_db = env_i32("DAB_SYNC_PROBE_IF_GAIN_DB", 0);
    device_config.bias_tee_enabled = env_i32("DAB_SYNC_PROBE_BIAS_TEE", 0);
    device_config.ppm_correction = env_i32("DAB_SYNC_PROBE_PPM", 0);

    stream_config.sample_rate = env_u32("DAB_SYNC_PROBE_SAMPLE_RATE", 2048000u);
    stream_config.center_freq_hz = env_u64("DAB_SYNC_PROBE_CENTER_FREQ_HZ", 220352000ull);
    stream_config.bandwidth_hz = env_u32("DAB_SYNC_PROBE_BANDWIDTH_HZ", 1536000u);
    stream_config.gain_db = env_i32("DAB_SYNC_PROBE_GAIN_DB", 40);
    audio_timeout_samples =
        (uint64_t)(env_f32("DAB_SDRPLAY_AUDIO_RESET_TIMEOUT_SECONDS", 2.0f) * (float)stream_config.sample_rate);
    if (audio_timeout_samples == 0u) {
        audio_timeout_samples = (uint64_t)stream_config.sample_rate * 2u;
    }

    {
        int argi;

        for (argi = 1; argi < argc; ++argi) {
            if (strcmp(argv[argi], "--freq") == 0 || strcmp(argv[argi], "-f") == 0) {
                if (argi + 1 >= argc || parse_u64_arg(argv[argi + 1], &stream_config.center_freq_hz) != 0) {
                    print_usage(argv[0]);
                    return 2;
                }
                freq_set = 1u;
                ++argi;
                continue;
            }
            if (strcmp(argv[argi], "--agc") == 0 || strcmp(argv[argi], "-a") == 0) {
                if (argi + 1 >= argc || parse_agc_arg(argv[argi + 1], &device_config.gain_mode) != 0) {
                    print_usage(argv[0]);
                    return 2;
                }
                ++argi;
                continue;
            }
            if (strcmp(argv[argi], "--sid") == 0 || strcmp(argv[argi], "-s") == 0) {
                if (argi + 1 >= argc || parse_u32_base0_arg(argv[argi + 1], &selected_audio_sid) != 0) {
                    print_usage(argv[0]);
                    return 2;
                }
                selected_audio_sid_valid = 1u;
                ++argi;
                continue;
            }
            if (strcmp(argv[argi], "--debug") == 0 || strcmp(argv[argi], "-d") == 0) {
                if (argi + 1 >= argc || parse_on_off_arg(argv[argi + 1], &debug_logs_enabled) != 0) {
                    print_usage(argv[0]);
                    return 2;
                }
                ++argi;
                continue;
            }
            if (strcmp(argv[argi], "--help") == 0 || strcmp(argv[argi], "-h") == 0) {
                print_usage(argv[0]);
                return 0;
            }

            print_usage(argv[0]);
            return 2;
        }
    }
    if (freq_set == 0u) {
        print_usage(argv[0]);
        return 2;
    }
    g_debug_logs = debug_logs_enabled != 0 ? 1 : 0;

#if !defined(DAB_HAVE_AUDIO_PLAYBACK)
    fprintf(stderr,
            "dab_sdrplay_audio requires PortAudio and either FDK-AAC or FAAD2 at build time\n");
    return 2;
#endif

    dab_null_detect_default_config(&detector_config);
    detector_config.fast_window_samples = env_u32("DAB_SYNC_PROBE_FAST_WINDOW_SAMPLES", 128u);
    detector_config.slow_window_samples = env_u32("DAB_SYNC_PROBE_SLOW_WINDOW_SAMPLES", 8192u);
    detector_config.sample_decimation = env_u32("DAB_SYNC_PROBE_SAMPLE_DECIMATION", 4u);
    detector_config.enter_ratio_threshold = env_f32("DAB_SYNC_PROBE_ENTER_RATIO_THRESHOLD", 0.35f);
    detector_config.exit_ratio_threshold = env_f32("DAB_SYNC_PROBE_EXIT_RATIO_THRESHOLD", 0.55f);
    detector_config.min_null_samples = env_u32("DAB_SYNC_PROBE_MIN_NULL_SAMPLES", 1800u);
    detector_config.max_null_samples = env_u32("DAB_SYNC_PROBE_MAX_NULL_SAMPLES", 4000u);
    detector_config.startup_ignore_samples = env_u32("DAB_SYNC_PROBE_STARTUP_IGNORE_SAMPLES", 0u);
    detector_config.max_merge_gap_samples = env_u32("DAB_SYNC_PROBE_MAX_MERGE_GAP_SAMPLES", 0u);
    if (dab_null_detect_init(&detector, &detector_config) != 0) {
        fprintf(stderr, "null detector init failed\n");
        return 2;
    }

    dab_prs_acquire_default_config(&acquire_config);
    acquire_config.acquire_window_samples = env_u32("DAB_SYNC_PROBE_ACQUIRE_WINDOW_SAMPLES",
                                                    acquire_config.acquire_window_samples);
    if (dab_prs_acquire_init(&acquire, &acquire_config) != 0) {
        fprintf(stderr, "prs acquire init failed\n");
        return 2;
    }

    dab_prs_score_default_config(&score_config);
    score_config.sample_rate = stream_config.sample_rate;
    score_config.guard_samples = acquire_config.guard_samples;
    score_config.fft_samples = acquire_config.fft_samples;
    if (dab_prs_score_init(&cp_score, &score_config) != 0) {
        fprintf(stderr, "prs score init failed\n");
        return 2;
    }

    dab_prs_fft_default_config(&fft_config);
    fft_config.fft_samples = acquire_config.fft_samples;
    fft_config.sample_rate = stream_config.sample_rate;
    if (dab_prs_fft_init(&fft, &fft_config) != 0) {
        fprintf(stderr, "prs fft init failed\n");
        return 2;
    }

    dab_sync_default_config(&sync_config);
    sync_config.acquire_timing_window_samples = acquire_config.acquire_window_samples;
    sync_config.track_timing_window_samples = env_u32("DAB_SYNC_PROBE_TRACK_WINDOW_SAMPLES",
                                                      sync_config.track_timing_window_samples);
    sync_config.acquire_prs_score_threshold = env_f32("DAB_SYNC_PROBE_ACQUIRE_PRS_SCORE_THRESHOLD",
                                                      ref_threshold);
    sync_config.track_prs_score_threshold = env_f32("DAB_SYNC_PROBE_TRACK_PRS_SCORE_THRESHOLD",
                                                    ref_threshold * 0.8f);
    sync_config.cfo_pi_proportional_gain = env_f32("DAB_SYNC_PROBE_CFO_PI_KP",
                                                   sync_config.cfo_pi_proportional_gain);
    sync_config.cfo_pi_integral_gain = env_f32("DAB_SYNC_PROBE_CFO_PI_KI",
                                               sync_config.cfo_pi_integral_gain);
    sync_config.cfo_pi_integral_limit_hz = env_f32("DAB_SYNC_PROBE_CFO_PI_I_LIMIT_HZ",
                                                   sync_config.cfo_pi_integral_limit_hz);
    if (dab_sync_init(&sync, &sync_config) != 0) {
        fprintf(stderr, "sync init failed\n");
        return 2;
    }
    if (dab_fic_init(&fic) != 0 || dab_msc_init(&msc) != 0) {
        fprintf(stderr, "fic/msc init failed\n");
        return 2;
    }

    dab_ofdm_default_config(&ofdm_config);
    if (dab_ofdm_init(&ofdm, &ofdm_config) != 0) {
        fprintf(stderr, "ofdm init failed\n");
        return 2;
    }

    fft_i = (float *)calloc(acquire_config.fft_samples, sizeof(*fft_i));
    fft_q = (float *)calloc(acquire_config.fft_samples, sizeof(*fft_q));
    active_i = (float *)calloc(DAB_MODE_I_ACTIVE_CARRIERS, sizeof(*active_i));
    active_q = (float *)calloc(DAB_MODE_I_ACTIVE_CARRIERS, sizeof(*active_q));
    raw_active_i = (float *)calloc(DAB_MODE_I_ACTIVE_CARRIERS, sizeof(*raw_active_i));
    raw_active_q = (float *)calloc(DAB_MODE_I_ACTIVE_CARRIERS, sizeof(*raw_active_q));
    prev_active_i = (float *)calloc(DAB_MODE_I_ACTIVE_CARRIERS, sizeof(*prev_active_i));
    prev_active_q = (float *)calloc(DAB_MODE_I_ACTIVE_CARRIERS, sizeof(*prev_active_q));
    diff_i = (float *)calloc(DAB_MODE_I_ACTIVE_CARRIERS, sizeof(*diff_i));
    diff_q = (float *)calloc(DAB_MODE_I_ACTIVE_CARRIERS, sizeof(*diff_q));
    deintl_i = (float *)calloc(DAB_MODE_I_DATA_CARRIERS, sizeof(*deintl_i));
    deintl_q = (float *)calloc(DAB_MODE_I_DATA_CARRIERS, sizeof(*deintl_q));
    soft_a = (int8_t *)calloc(DAB_MODE_I_DATA_CARRIERS, sizeof(*soft_a));
    soft_b = (int8_t *)calloc(DAB_MODE_I_DATA_CARRIERS, sizeof(*soft_b));
    fic_softbits = (int8_t *)calloc(DAB_TFRAME_FIC_SOFTBITS, sizeof(*fic_softbits));
    msc_softbits = (int8_t *)calloc(DAB_TFRAME_MSC_SOFTBITS, sizeof(*msc_softbits));
    frame_softbits = (int8_t *)calloc(DAB_TFRAME_TOTAL_SOFTBITS, sizeof(*frame_softbits));
    if (fft_i == 0 || fft_q == 0 || active_i == 0 || active_q == 0 ||
        raw_active_i == 0 || raw_active_q == 0 ||
        prev_active_i == 0 || prev_active_q == 0 || diff_i == 0 || diff_q == 0 ||
        deintl_i == 0 || deintl_q == 0 || soft_a == 0 || soft_b == 0 ||
        fic_softbits == 0 || msc_softbits == 0 || frame_softbits == 0) {
        fprintf(stderr, "alloc failed\n");
        goto cleanup;
    }
    ofdm.prev_active_i = prev_active_i;
    ofdm.prev_active_q = prev_active_q;
    tframe_buffers.fic_softbits = fic_softbits;
    tframe_buffers.msc_softbits = msc_softbits;
    tframe_buffers.frame_softbits = frame_softbits;
    if (dab_tframe_init(&tframe, &tframe_buffers) != 0) {
        fprintf(stderr, "tframe init failed\n");
        goto cleanup;
    }

    if (history_init(&history, env_u32("DAB_SYNC_PROBE_HISTORY_SAMPLES", 262144u)) != 0) {
        fprintf(stderr, "history init failed\n");
        goto cleanup;
    }

    if (dump_ref_samples_path != 0 && dump_ref_samples_path[0] != '\0') {
        ref_samples_fp = fopen(dump_ref_samples_path, "wb");
        if (ref_samples_fp == 0) {
            fprintf(stderr, "failed to open ref samples dump: %s\n", dump_ref_samples_path);
            goto cleanup;
        }
    }
    if (dump_prs_fft_gnuplot_path != 0 && dump_prs_fft_gnuplot_path[0] != '\0') {
        prs_fft_gnuplot_fp = fopen(dump_prs_fft_gnuplot_path, "wb");
        if (prs_fft_gnuplot_fp == 0) {
            fprintf(stderr, "failed to open prs fft gnuplot dump: %s\n", dump_prs_fft_gnuplot_path);
            goto cleanup;
        }
    }

    fprintf(stderr, "open device\n");
    if (dab_sdr_device_open(&params, &device) != DAB_SDR_STATUS_OK || device == 0) {
        fprintf(stderr, "open failed\n");
        goto cleanup;
    }
    fprintf(stderr, "configure device\n");
    if (dab_sdrplay_configure_device(device, &device_config) != DAB_SDR_STATUS_OK) {
        fprintf(stderr, "device config failed\n");
        goto cleanup;
    }
    fprintf(stderr, "configure stream\n");
    if (device->vtbl->configure(device, &stream_config) != DAB_SDR_STATUS_OK) {
        fprintf(stderr, "stream config failed\n");
        goto cleanup;
    }
    fprintf(stderr, "open stream\n");
    if (device->vtbl->open_stream(device, &stream) != DAB_SDR_STATUS_OK || stream == 0) {
        fprintf(stderr, "open stream failed\n");
        goto cleanup;
    }
    fprintf(stderr, "start stream\n");
    if (stream->vtbl->start(stream) != DAB_SDR_STATUS_OK) {
        fprintf(stderr, "start failed\n");
        goto cleanup;
    }

    printf("sdrplay-audio start sample_rate=%u center_freq_hz=%llu agc=%s debug=%s simd=%s fft=%s ref_threshold=%.3f",
           stream_config.sample_rate,
           (unsigned long long)stream_config.center_freq_hz,
           device_config.gain_mode == DAB_SDRPLAY_GAIN_MODE_AUTO ? "on" : "off",
           g_debug_logs != 0 ? "on" : "off",
           simd_ops != 0 ? simd_ops->name : "unknown",
           fft.backend_name != 0 ? fft.backend_name : "unknown",
           ref_threshold);
    if (selected_audio_sid_valid != 0u) {
        printf(" sid=0x%08x", selected_audio_sid);
    }
    printf("\n");

    while (g_stop_requested == 0) {
        dab_sdr_block_view_t hist_view;
        const uint8_t locked_now = (uint8_t)(sync.state.locked != 0);

        if (prev_locked != locked_now) {
            if (locked_now != 0u) {
                if (reacquire_report_pending != 0u) {
                    printf("reacquired frame_start=%llu cfo=%.3f ifo=%d\n",
                           (unsigned long long)sync.state.frame_start_sample,
                           sync.state.coarse_freq_error_hz,
                           sync.state.carrier_offset_bins);
                    reacquire_report_pending = 0u;
                }
            } else {
                printf("lost-lock bad_frames=%u good_frames=%u prs_score=%.6f\n",
                       sync.state.bad_frames,
                       sync.state.good_frames,
                       sync.state.prs_score);
                reset_runtime_state(&detector,
                                    &sync,
                                    &ofdm,
                                    &tframe,
                                    &fic,
                                    &msc,
                                    &history,
                                    &pending,
                                    &last_predicted_fft_start,
                                    &audio,
                                    audio_debug_enabled,
                                    &announced_audio_sid);
                reacquire_report_pending = 1u;
                playing_reported = 0u;
                waiting_for_audio_reported = 0u;
                samples_since_audio = 0u;
            }
            prev_locked = (uint8_t)(sync.state.locked != 0);
        }

        if (stream->vtbl->read(stream, &src_block) != DAB_SDR_STATUS_OK) {
            fprintf(stderr, "read failed at block %llu\n", (unsigned long long)blocks_read);
            goto cleanup;
        }
        blocks_read++;
        samples_since_audio += src_block.sample_count;
        if (ref_samples_fp != 0 &&
            append_ref_samples_file(ref_samples_fp,
                                    &ref_samples_scratch,
                                    &ref_samples_scratch_capacity,
                                    &src_block) != 0) {
            fprintf(stderr, "failed to write ref samples dump\n");
            goto cleanup;
        }
        history_append(&history, &src_block);

        if (dab_null_detect_process_i16(&detector, &src_block, &event) < 0) {
            fprintf(stderr, "null detector failed at block %llu\n", (unsigned long long)blocks_read);
            goto cleanup;
        }
        if (playing_reported != 0u && samples_since_audio >= audio_timeout_samples) {
            printf("audio-timeout sid=0x%08x timeout_samples=%llu locked=%d\n",
                   selected_audio_sid,
                   (unsigned long long)samples_since_audio,
                   sync.state.locked);
            reset_runtime_state(&detector,
                                &sync,
                                &ofdm,
                                &tframe,
                                &fic,
                                &msc,
                                &history,
                                &pending,
                                &last_predicted_fft_start,
                                &audio,
                                audio_debug_enabled,
                                &announced_audio_sid);
            reacquire_report_pending = 1u;
            playing_reported = 0u;
            waiting_for_audio_reported = 0u;
            samples_since_audio = 0u;
            prev_locked = 0u;
            continue;
        }
        if (playing_reported == 0u &&
            selected_audio_sid_valid != 0u &&
            sync.state.locked != 0 &&
            samples_since_audio >= audio_timeout_samples) {
#if defined(DAB_HAVE_AUDIO_PLAYBACK)
            dab_msc_dabplus_debug_t audio_debug = {0};

            if (dab_msc_get_primary_dabplus_debug(&msc, selected_audio_sid, &audio_debug) == 0) {
                printf("audio-recovery-reset sid=0x%08x timeout_samples=%llu sync=%u superframe_valid=%u attempts=%u failures=%u\n",
                       selected_audio_sid,
                       (unsigned long long)samples_since_audio,
                       audio_debug.synchronized,
                       audio_debug.superframe_valid,
                       audio_debug.decode_attempts,
                       audio_debug.decode_failures);
            } else {
                printf("audio-recovery-reset sid=0x%08x timeout_samples=%llu sync=0 superframe_valid=0 attempts=0 failures=0\n",
                       selected_audio_sid,
                       (unsigned long long)samples_since_audio);
            }
#else
            printf("audio-recovery-reset sid=0x%08x timeout_samples=%llu\n",
                   selected_audio_sid,
                   (unsigned long long)samples_since_audio);
#endif
            reset_runtime_state(&detector,
                                &sync,
                                &ofdm,
                                &tframe,
                                &fic,
                                &msc,
                                &history,
                                &pending,
                                &last_predicted_fft_start,
                                &audio,
                                audio_debug_enabled,
                                &announced_audio_sid);
            reacquire_report_pending = 1u;
            playing_reported = 0u;
            waiting_for_audio_reported = 0u;
            samples_since_audio = 0u;
            prev_locked = 0u;
            continue;
        }

        if (event.length != 0u && !sync.state.locked) {
            if (dab_prs_candidate_from_null(&acquire, &event, &pending.candidate) == 0) {
                pending.pending = 1;
                pending.predicted = 0;
                pending.event = event;
                DAB_DEBUG_LOG("null sample_index=%llu length=%u candidate_fft=%llu search_start=%llu search_end=%llu\n",
                              (unsigned long long)event.sample_index,
                              event.length,
                              (unsigned long long)pending.candidate.prs_fft_start_sample,
                              (unsigned long long)dab_prs_candidate_search_start_sample(&pending.candidate),
                              (unsigned long long)dab_prs_candidate_search_end_sample(&pending.candidate));
            }
        }

        if (!pending.pending && sync.state.locked) {
            dab_prs_candidate_t predicted_candidate = {0};

            init_predicted_candidate(&acquire, &sync, &predicted_candidate);
            if (predicted_candidate.prs_fft_start_sample != last_predicted_fft_start) {
                pending.pending = 1;
                pending.predicted = 1;
                pending.candidate = predicted_candidate;
                pending.event = (dab_null_event_t){0};
                last_predicted_fft_start = predicted_candidate.prs_fft_start_sample;
                DAB_DEBUG_LOG("predict candidate_fft=%llu search_start=%llu search_end=%llu\n",
                              (unsigned long long)pending.candidate.prs_fft_start_sample,
                              (unsigned long long)dab_prs_candidate_search_start_sample(&pending.candidate),
                              (unsigned long long)dab_prs_candidate_search_end_sample(&pending.candidate));
            }
        }

        if (!pending.pending) {
            continue;
        }

        {
            uint64_t required_end = dab_prs_candidate_search_end_sample(&pending.candidate) + acquire_config.fft_samples;
            if (sync.state.locked) {
                required_end += ((uint64_t)DAB_TFRAME_TOTAL_SYMBOLS * acquire_config.symbol_samples);
            }
            if (!history_contains_sample_range(&history,
                                               dab_prs_candidate_search_start_sample(&pending.candidate),
                                               required_end)) {
                continue;
            }
        }

        hist_view = history_view(&history);
        {
            dab_prs_result_t coarse = {0};
            dab_prs_reference_score_t ref_score = {0};
            float prs_plot_i[DAB_MODE_I_ACTIVE_CARRIERS];
            float prs_plot_q[DAB_MODE_I_ACTIVE_CARRIERS];
            int coarse_rc = dab_prs_score_search_i16(&cp_score, &hist_view, &pending.candidate, &coarse);

            if (coarse_rc == 1) {
                float cfo_for_fft = coarse.coarse_freq_error_hz;
                if (sync.state.locked && sync.state.bad_frames == 0u) {
                    cfo_for_fft = sync.state.coarse_freq_error_hz;
                }
                if (dab_prs_score_symbol_i16_mode_i_diff_cfo(&fft,
                                                             &hist_view,
                                                             coarse.prs_fft_start_sample,
                                                             cfo_for_fft,
                                                             fft_i,
                                                             fft_q,
                                                             &ref_score) == 0) {
                    int sync_rc;
                    float applied_prs_timing = 0.0f;
                    uint64_t prs_fft_start_sample =
                        apply_prs_fine_timing(coarse.prs_fft_start_sample,
                                              ref_score.fine_timing_error_samples,
                                              prs_fine_timing_limit,
                                              &applied_prs_timing);
                    if (prs_fft_start_sample != coarse.prs_fft_start_sample &&
                        dab_prs_score_symbol_i16_mode_i_diff_cfo(&fft,
                                                                 &hist_view,
                                                                 prs_fft_start_sample,
                                                                 cfo_for_fft,
                                                                 fft_i,
                                                                 fft_q,
                                                                 &ref_score) != 0) {
                        pending.pending = 0;
                        pending.predicted = 0;
                        continue;
                    }
                    if (ref_score.carrier_offset_bins != 0) {
                        (void)dab_prs_fft_apply_bin_shift(fft_i,
                                                          fft_q,
                                                          acquire_config.fft_samples,
                                                          -ref_score.carrier_offset_bins);
                    }
                    if (!prs_fft_dump_written &&
                        dump_prs_fft_path != 0 &&
                        dump_prs_fft_path[0] != '\0') {
                        memcpy(prs_fft_dump.bins[0], fft_i, sizeof(prs_fft_dump.bins[0]));
                        memcpy(prs_fft_dump.bins[1], fft_q, sizeof(prs_fft_dump.bins[1]));
                        if (write_binary_file(dump_prs_fft_path,
                                              &prs_fft_dump,
                                              sizeof(prs_fft_dump)) == 0) {
                            prs_fft_dump_written = 1;
                            fprintf(stderr, "prs_fft_dump=%s\n", dump_prs_fft_path);
                        }
                    }
                    if (dab_ofdm_extract_active_carriers_mode_i(fft_i, fft_q, active_i, active_q) != 0) {
                        pending.pending = 0;
                        pending.predicted = 0;
                        continue;
                    }
                    phase_correct_prs_for_plot(&ofdm, active_i, active_q, prs_plot_i, prs_plot_q);
                    if (prs_fft_gnuplot_fp != 0 &&
                        append_fft_gnuplot_frame(prs_fft_gnuplot_fp,
                                                 prs_fft_gnuplot_frame++,
                                                 "prs",
                                                 prs_fft_start_sample,
                                                 prs_plot_i,
                                                 prs_plot_q,
                                                 DAB_MODE_I_ACTIVE_CARRIERS) != 0) {
                        fprintf(stderr, "failed to append prs fft gnuplot dump\n");
                        goto cleanup;
                    }
                    DAB_DEBUG_LOG("prs fft_start=%llu cp_score=%.6f ref_score=%.6f ifo=%d cfo_meas=%.3f fine_timing=%.3f applied=%.0f mode=%d locked=%d\n",
                                  (unsigned long long)prs_fft_start_sample,
                                  coarse.prs_score,
                                  ref_score.score,
                                  ref_score.carrier_offset_bins,
                                  coarse.coarse_freq_error_hz,
                                  ref_score.fine_timing_error_samples,
                                  applied_prs_timing,
                                  sync.state.mode,
                                  sync.state.locked);

                    if (sync.state.locked) {
                        sync_rc = dab_sync_on_prs_track(&sync,
                                                        prs_fft_start_sample,
                                                        ref_score.score,
                                                        coarse.coarse_freq_error_hz,
                                                        ref_score.carrier_offset_bins);
                    } else {
                        dab_sync_on_null_candidate(&sync, &pending.event);
                        sync_rc = dab_sync_on_prs_acquire(&sync,
                                                          prs_fft_start_sample,
                                                          ref_score.score,
                                                          coarse.coarse_freq_error_hz,
                                                          ref_score.carrier_offset_bins);
                    }

                    if (!sync.state.locked) {
                        last_predicted_fft_start = 0u;
                        dab_ofdm_reset(&ofdm);
                        dab_tframe_reset(&tframe);
                        if (pending.predicted) {
                            DAB_DEBUG_LOG("track-loss source=predict bad=%u mode=%d locked=%d\n",
                                          sync.state.bad_frames,
                                          sync.state.mode,
                                          sync.state.locked);
                        }
                    }

                    if (ref_score.score >= ref_threshold) {
                        DAB_DEBUG_LOG("sync-update rc=%d source=%s frame_start=%llu prs_score=%.6f bad=%u good=%u mode=%d locked=%d timing_error=%d cfo_out=%.3f cfo_pi=%.3f cfo_err=%.3f ifo=%d\n",
                                      sync_rc,
                                      pending.predicted ? "predict" : "null",
                                      (unsigned long long)sync.state.frame_start_sample,
                                      sync.state.prs_score,
                                      sync.state.bad_frames,
                                      sync.state.good_frames,
                                      sync.state.mode,
                                      sync.state.locked,
                                      sync.state.timing_error_samples,
                                      sync.state.coarse_freq_error_hz,
                                      sync.state.cfo_integrator_hz,
                                      sync.state.fine_freq_error_hz,
                                      sync.state.carrier_offset_bins);
                    }

                    {
                        dab_ofdm_buffers_t ofdm_buffers;
                        dab_ofdm_result_t ofdm_result;
                        uint64_t data_fft_start;
                        dab_tframe_result_t tframe_result;
                        char data_kind[32];
                        char diff_kind[32];
                        unsigned int symbol_idx;

                        ofdm_buffers.active_i = active_i;
                        ofdm_buffers.active_q = active_q;
                        ofdm_buffers.diff_i = diff_i;
                        ofdm_buffers.diff_q = diff_q;
                        ofdm_buffers.deintl_i = deintl_i;
                        ofdm_buffers.deintl_q = deintl_q;
                        ofdm_buffers.soft_a = soft_a;
                        ofdm_buffers.soft_b = soft_b;

                        dab_ofdm_reset(&ofdm);
                        dab_tframe_reset(&tframe);
                        (void)dab_ofdm_process_mode_i(&ofdm,
                                                      fft_i,
                                                      fft_q,
                                                      prs_fft_start_sample,
                                                      DAB_OFDM_SYMBOL_PRS,
                                                      &ofdm_buffers,
                                                      &ofdm_result);
                        (void)dab_ofdm_extract_active_carriers_mode_i(fft_i,
                                                                      fft_q,
                                                                      raw_active_i,
                                                                      raw_active_q);
                        memcpy(fic_active_dump.carriers[0][0],
                               active_i,
                               sizeof(active_i[0]) * 1537u);
                        memcpy(fic_active_dump.carriers[0][1],
                               active_q,
                               sizeof(active_q[0]) * 1537u);
                        memcpy(fic_raw_active_dump.carriers[0][0],
                               raw_active_i,
                               sizeof(raw_active_i[0]) * 1537u);
                        memcpy(fic_raw_active_dump.carriers[0][1],
                               raw_active_q,
                               sizeof(raw_active_q[0]) * 1537u);

                        for (symbol_idx = 0u; symbol_idx < DAB_TFRAME_TOTAL_SYMBOLS; ++symbol_idx) {
                            data_fft_start = prs_fft_start_sample +
                                             ((uint64_t)(symbol_idx + 1u) * acquire_config.symbol_samples);
                            if (!history_contains_sample_range(&history,
                                                               data_fft_start,
                                                               data_fft_start + acquire_config.fft_samples)) {
                                break;
                            }
                            if (dab_prs_fft_execute_i16_cfo(&fft,
                                                            &hist_view,
                                                            data_fft_start,
                                                            cfo_for_fft,
                                                            fft_i,
                                                            fft_q) != 0) {
                                break;
                            }
                            if (ref_score.carrier_offset_bins != 0) {
                                (void)dab_prs_fft_apply_bin_shift(fft_i,
                                                                  fft_q,
                                                                  acquire_config.fft_samples,
                                                                  -ref_score.carrier_offset_bins);
                            }
                            if (symbol_idx == 0u &&
                                dab_ofdm_extract_active_carriers_mode_i(fft_i,
                                                                        fft_q,
                                                                        raw_active_i,
                                                                        raw_active_q) != 0) {
                                break;
                            }
                            if (dab_ofdm_process_mode_i(&ofdm,
                                                        fft_i,
                                                        fft_q,
                                                        data_fft_start,
                                                        DAB_OFDM_SYMBOL_DATA,
                                                        &ofdm_buffers,
                                                        &ofdm_result) != 0) {
                                break;
                            }
                            if (prs_fft_gnuplot_fp != 0) {
                                snprintf(data_kind, sizeof(data_kind), "data%u", symbol_idx);
                            }
                            if (prs_fft_gnuplot_fp != 0 &&
                                append_fft_gnuplot_frame(prs_fft_gnuplot_fp,
                                                         prs_fft_gnuplot_frame++,
                                                         data_kind,
                                                         data_fft_start,
                                                         active_i,
                                                         active_q,
                                                         DAB_MODE_I_ACTIVE_CARRIERS) != 0) {
                                fprintf(stderr, "failed to append data fft gnuplot dump\n");
                                goto cleanup;
                            }
                            if (prs_fft_gnuplot_fp != 0 && ofdm_result.have_diff) {
                                snprintf(diff_kind, sizeof(diff_kind), "diff%u", symbol_idx);
                                if (append_fft_gnuplot_frame(prs_fft_gnuplot_fp,
                                                             prs_fft_gnuplot_frame++,
                                                             diff_kind,
                                                             data_fft_start,
                                                             diff_i,
                                                             diff_q,
                                                             DAB_MODE_I_DATA_CARRIERS) != 0) {
                                    fprintf(stderr, "failed to append diff fft gnuplot dump\n");
                                    goto cleanup;
                                }
                            }
                            (void)dab_ofdm_extract_active_carriers_mode_i(fft_i,
                                                                          fft_q,
                                                                          raw_active_i,
                                                                          raw_active_q);
                            if (ofdm_result.have_diff) {
                                const float cpe_deg =
                                    ofdm_result.have_softbits
                                        ? decision_directed_phase_error_deg(deintl_i,
                                                                            deintl_q,
                                                                            DAB_MODE_I_DATA_CARRIERS)
                                        : 0.0f;
                                if (ofdm_result.have_softbits && apply_data_cpe_correction) {
                                    rotate_complex_f32(deintl_i,
                                                       deintl_q,
                                                       DAB_MODE_I_DATA_CARRIERS,
                                                       -cpe_deg);
                                    if (dab_ofdm_qpsk_soft_decode_mode_i(deintl_i,
                                                                         deintl_q,
                                                                         soft_a,
                                                                         soft_b) != 0) {
                                        break;
                                    }
                                }
                                const float diff_power = (diff_i[0] * diff_i[0]) + (diff_q[0] * diff_q[0]);
                                DAB_DEBUG_LOG("ofdm data_fft_start=%llu diff0_re=%.6f diff0_im=%.6f diff0_power=%.6f data_cpe_deg=%.3f data_cpe_corr=%d",
                                              (unsigned long long)data_fft_start,
                                              diff_i[0],
                                              diff_q[0],
                                              diff_power,
                                              cpe_deg,
                                              apply_data_cpe_correction);
                                if (ofdm_result.have_softbits) {
                                        DAB_DEBUG_LOG(" soft0=(%d,%d) soft1=(%d,%d)",
                                                      (int)soft_a[0],
                                                      (int)soft_b[0],
                                                      (int)soft_a[1],
                                                      (int)soft_b[1]);
                                    if (dab_tframe_push_symbol(&tframe,
                                                               data_fft_start,
                                                               soft_a,
                                                               soft_b,
                                                               &tframe_result) == 0) {
                                        if (tframe_result.symbol_index < DAB_TFRAME_FIC_SYMBOLS) {
                                            memcpy(fic_active_dump.carriers[tframe_result.symbol_index + 1u][0],
                                                   active_i,
                                                   sizeof(active_i[0]) * 1537u);
                                            memcpy(fic_active_dump.carriers[tframe_result.symbol_index + 1u][1],
                                                   active_q,
                                                   sizeof(active_q[0]) * 1537u);
                                            memcpy(fic_raw_active_dump.carriers[tframe_result.symbol_index + 1u][0],
                                                   raw_active_i,
                                                   sizeof(raw_active_i[0]) * 1537u);
                                            memcpy(fic_raw_active_dump.carriers[tframe_result.symbol_index + 1u][1],
                                                   raw_active_q,
                                                   sizeof(raw_active_q[0]) * 1537u);
                                            memcpy(fic_diff_dump.carriers[tframe_result.symbol_index][0],
                                                   diff_i,
                                                   sizeof(diff_i[0]) * 1537u);
                                            memcpy(fic_diff_dump.carriers[tframe_result.symbol_index][1],
                                                   diff_q,
                                                   sizeof(diff_q[0]) * 1537u);
                                            memcpy(fic_carrier_dump.carriers[tframe_result.symbol_index][0],
                                                   deintl_i,
                                                   sizeof(deintl_i[0]) * 1536u);
                                            memcpy(fic_carrier_dump.carriers[tframe_result.symbol_index][1],
                                                   deintl_q,
                                                   sizeof(deintl_q[0]) * 1536u);
                                            memcpy(fic_symbol_dump.softbits[tframe_result.symbol_index][0],
                                                   soft_a,
                                                   sizeof(soft_a[0]) * 1536u);
                                            memcpy(fic_symbol_dump.softbits[tframe_result.symbol_index][1],
                                                   soft_b,
                                                   sizeof(soft_b[0]) * 1536u);
                                        }
                                        DAB_DEBUG_LOG(" tframe_symbol=%u frame_complete=%d",
                                                      tframe_result.symbol_index,
                                                      tframe_result.frame_complete);
                                        if (tframe_result.frame_complete) {
                                            if (dab_fic_process_frame(&fic,
                                                                      tframe_result.frame_start_sample,
                                                                      fic_softbits,
                                                                      DAB_TFRAME_FIC_SOFTBITS) == 0 &&
                                                dab_msc_process_frame_with_fic(&msc,
                                                                               tframe_result.frame_start_sample,
                                                                               msc_softbits,
                                                                               DAB_TFRAME_MSC_SOFTBITS,
                                                                               &fic.repo.current) == 0) {
                                                const dab_msc_decoded_component_t *audio_component = 0;
                                                const dab_fic_label_t *label = 0;
#if defined(DAB_HAVE_AUDIO_PLAYBACK)
                                                dab_msc_dabplus_superframe_t superframe = {0};
                                                dab_msc_dabplus_debug_t audio_debug = {0};
#endif

                                                DAB_DEBUG_LOG(" fic_frames=%u fic_blocks=%u fic_valid_fibs=%u",
                                                              fic.frames_seen,
                                                              fic.fic_blocks_decoded_last_frame,
                                                              fic.valid_fibs_last_frame);
                                                {
                                                    uint32_t fic_block_index;
                                                    for (fic_block_index = 0u;
                                                         fic_block_index < DAB_FIC_BLOCKS_PER_FRAME;
                                                         ++fic_block_index) {
                                                        const dab_fic_debug_block_t *info =
                                                            &fic.block_info_last_frame[fic_block_index];
                                                        DAB_DEBUG_LOG(" fic_block%u=(valid=%u pol=%u pack=%u scr=%u byte=%u)",
                                                                      fic_block_index,
                                                                      info->valid_fibs,
                                                                      info->polarity_variant,
                                                                      info->pack_variant,
                                                                      info->scramble_variant,
                                                                      info->byte_variant);
                                                    }
                                                }
                                                DAB_DEBUG_LOG(" msc_frames=%u msc_edge=(%d,%d)",
                                                              msc.frames_seen,
                                                              (int)msc.first_softbit,
                                                              (int)msc.last_softbit);
                                                if (fic.valid_fibs_last_frame > 0u) {
                                                    DAB_DEBUG_LOG("\n");
                                                    if (g_debug_logs != 0) {
                                                        print_fic_repo_summary(&fic.repo);
                                                    }
                                                }
                                                audio_component =
                                                    selected_audio_sid_valid != 0u
                                                        ? find_selected_audio_component(&msc, selected_audio_sid)
                                                        : 0;
                                                if (audio_component == 0 && selected_audio_sid_valid == 0u) {
                                                    audio_component = find_first_audio_component(&msc);
                                                    if (audio_component != 0) {
                                                        selected_audio_sid = audio_component->sid;
                                                        selected_audio_sid_valid = 1u;
                                                    }
                                                }
                                                if (audio_component != 0 &&
                                                    (!selected_audio_sid_valid || selected_audio_sid != audio_component->sid)) {
                                                    selected_audio_sid = audio_component->sid;
                                                    selected_audio_sid_valid = 1u;
                                                }
                                                if (selected_audio_sid_valid != 0) {
                                                    label = fic_repo_find_label(&fic.repo.current,
                                                                                DAB_FIC_LABEL_SCOPE_SERVICE,
                                                                                1u,
                                                                                selected_audio_sid,
                                                                                0u,
                                                                                0u);
                                                }
                                                if (selected_audio_sid_valid != 0 &&
                                                    announced_audio_sid != selected_audio_sid) {
                                                    announced_audio_sid = selected_audio_sid;
                                                    printf("\nservice-discovered sid=0x%08x", selected_audio_sid);
                                                    if (label != 0 && label->text[0] != '\0') {
                                                        printf(" label=\"%s\"", label->text);
                                                    }
                                                    printf("\n");
                                                    waiting_for_audio_reported = 0u;
                                                }
#if defined(DAB_HAVE_AUDIO_PLAYBACK)
                                                if (selected_audio_sid_valid != 0 &&
                                                    dab_msc_get_primary_dabplus_superframe(&msc,
                                                                                           selected_audio_sid,
                                                                                           &superframe) == 0 &&
                                                    superframe.generation != audio.last_generation) {
                                                    if (dab_audio_playback_consume_superframe(&audio, &superframe) != 0) {
                                                        goto cleanup;
                                                    }
                                                    samples_since_audio = 0u;
                                                    if (playing_reported == 0u) {
                                                        const dab_fic_label_t *label =
                                                            fic_repo_find_label(&fic.repo.current,
                                                                                DAB_FIC_LABEL_SCOPE_SERVICE,
                                                                                1u,
                                                                                selected_audio_sid,
                                                                                0u,
                                                                                0u);

                                                        printf("playing sid=0x%08x", selected_audio_sid);
                                                        if (label != 0 && label->text[0] != '\0') {
                                                            printf(" label=\"%s\"", label->text);
                                                        }
                                                        printf("\n");
                                                        playing_reported = 1u;
                                                        waiting_for_audio_reported = 0u;
                                                    }
                                                } else if (selected_audio_sid_valid != 0 &&
                                                           playing_reported == 0u &&
                                                           dab_msc_get_primary_dabplus_debug(&msc,
                                                                                            selected_audio_sid,
                                                                                            &audio_debug) == 0 &&
                                                           waiting_for_audio_reported == 0u) {
                                                    printf("waiting-for-audio sid=0x%08x", selected_audio_sid);
                                                    if (label != 0 && label->text[0] != '\0') {
                                                        printf(" label=\"%s\"", label->text);
                                                    }
                                                    printf(" sync=%u superframe_valid=%u attempts=%u failures=%u\n",
                                                           audio_debug.synchronized,
                                                           audio_debug.superframe_valid,
                                                           audio_debug.decode_attempts,
                                                           audio_debug.decode_failures);
                                                    waiting_for_audio_reported = 1u;
                                                }
#endif
                                                if (!fic_dump_written &&
                                                    dump_fic_path != 0 &&
                                                    dump_fic_path[0] != '\0' &&
                                                    write_binary_file(dump_fic_path,
                                                                      fic_softbits,
                                                                      sizeof(*fic_softbits) * DAB_TFRAME_FIC_SOFTBITS) == 0) {
                                                    fic_dump_written = 1;
                                                    DAB_DEBUG_LOG(" fic_dump=%s", dump_fic_path);
                                                }
                                                if (!fic_symbols_dump_written &&
                                                    dump_fic_symbols_path != 0 &&
                                                    dump_fic_symbols_path[0] != '\0' &&
                                                    write_binary_file(dump_fic_symbols_path,
                                                                      &fic_symbol_dump,
                                                                      sizeof(fic_symbol_dump)) == 0) {
                                                    fic_symbols_dump_written = 1;
                                                    DAB_DEBUG_LOG(" fic_symbols_dump=%s", dump_fic_symbols_path);
                                                }
                                                if (!fic_carriers_dump_written &&
                                                    dump_fic_carriers_path != 0 &&
                                                    dump_fic_carriers_path[0] != '\0' &&
                                                    write_binary_file(dump_fic_carriers_path,
                                                                      &fic_carrier_dump,
                                                                      sizeof(fic_carrier_dump)) == 0) {
                                                    fic_carriers_dump_written = 1;
                                                    DAB_DEBUG_LOG(" fic_carriers_dump=%s", dump_fic_carriers_path);
                                                }
                                                if (!fic_diff_dump_written &&
                                                    dump_fic_diff_path != 0 &&
                                                    dump_fic_diff_path[0] != '\0' &&
                                                    write_binary_file(dump_fic_diff_path,
                                                                      &fic_diff_dump,
                                                                      sizeof(fic_diff_dump)) == 0) {
                                                    fic_diff_dump_written = 1;
                                                    DAB_DEBUG_LOG(" fic_diff_dump=%s", dump_fic_diff_path);
                                                }
                                                if (!fic_active_dump_written &&
                                                    dump_fic_active_path != 0 &&
                                                    dump_fic_active_path[0] != '\0' &&
                                                    write_binary_file(dump_fic_active_path,
                                                                      &fic_active_dump,
                                                                      sizeof(fic_active_dump)) == 0) {
                                                    fic_active_dump_written = 1;
                                                    DAB_DEBUG_LOG(" fic_active_dump=%s", dump_fic_active_path);
                                                }
                                                if (!fic_raw_active_dump_written &&
                                                    dump_fic_raw_active_path != 0 &&
                                                    dump_fic_raw_active_path[0] != '\0' &&
                                                    write_binary_file(dump_fic_raw_active_path,
                                                                      &fic_raw_active_dump,
                                                                      sizeof(fic_raw_active_dump)) == 0) {
                                                    fic_raw_active_dump_written = 1;
                                                    DAB_DEBUG_LOG(" fic_raw_active_dump=%s", dump_fic_raw_active_path);
                                                }
                                            }
                                        }
                                    }
                                }
                                DAB_DEBUG_LOG("\n");
                            }
                        }
                    }
                }
            }
        }

        pending.pending = 0;
        pending.predicted = 0;
    }

    dropped_blocks = dab_sdrplay_stream_dropped_blocks(stream);
    printf("summary blocks=%llu dropped_blocks=%u mode=%d locked=%d frame_start=%llu prs_score=%.6f cfo=%.3f ifo=%d\n",
           (unsigned long long)blocks_read,
           dropped_blocks,
           sync.state.mode,
           sync.state.locked,
           (unsigned long long)sync.state.frame_start_sample,
           sync.state.prs_score,
           sync.state.coarse_freq_error_hz,
           sync.state.carrier_offset_bins);
    rc = 0;

cleanup:
#if defined(DAB_HAVE_AUDIO_PLAYBACK)
    dab_audio_playback_close(&audio);
#endif
    if (prs_fft_gnuplot_fp != 0) {
        fclose(prs_fft_gnuplot_fp);
    }
    if (ref_samples_fp != 0) {
        fclose(ref_samples_fp);
    }
    free(ref_samples_scratch);
    if (stream != 0) {
        (void)stream->vtbl->stop(stream);
        dab_sdr_stream_close(stream);
    }
    if (device != 0) {
        dab_sdr_device_close(device);
    }
    history_destroy(&history);
    free(frame_softbits);
    free(msc_softbits);
    free(fic_softbits);
    free(soft_b);
    free(soft_a);
    free(deintl_q);
    free(deintl_i);
    free(diff_q);
    free(diff_i);
    free(prev_active_q);
    free(prev_active_i);
    free(raw_active_q);
    free(raw_active_i);
    free(active_q);
    free(active_i);
    free(fft_q);
    free(fft_i);
    return rc;
}
