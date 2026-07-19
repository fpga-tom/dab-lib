#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "dab/core/fic.h"
#include "dab/core/msc.h"
#include "dab/core/null_detect.h"
#include "dab/core/ofdm.h"
#include "dab/core/prs_acquire.h"
#include "dab/core/prs_fft.h"
#include "dab/core/prs_score.h"
#include "dab/core/sample_history.h"
#include "dab/core/sync.h"
#include "dab/core/tframe.h"
#include "dab/simd/simd.h"
#include "dab.h"
#include "audio_playback.h"
#include "ref_impl_backend_hooks.h"

void unflatten_tframe(tframe_t *tframe, tframe_raw_t *tframe_raw);
void unflatten_subch(tframe_t *tframe, tframe_raw_t *tframe_raw, dab_subch_runtime_t *subch_rt);
int subch_read_cif(dab_subch_runtime_t *subch_rt, uint8_t *data, int cif);

typedef dab_sample_history_t history_buffer_t;

typedef struct {
    int pending;
    int predicted;
    dab_null_event_t event;
    dab_prs_candidate_t candidate;
} pending_candidate_t;

typedef struct {
    uint8_t valid;
    uint8_t resolved;
    uint8_t subchid;
    uint32_t sid;
} selected_stream_t;

typedef struct {
    uint8_t valid;
    uint8_t subchid;
    uint32_t sid;
    uint16_t byte_count[DAB_MSC_CIFS_PER_FRAME];
    const uint8_t *data[DAB_MSC_CIFS_PER_FRAME];
} ours_stream_payload_t;

typedef struct {
    uint8_t valid;
    uint8_t au_count;
    uint64_t sequence;
    uint32_t au_length[DAB_MSC_DABPLUS_MAX_AUS];
    uint8_t au_data[DAB_MSC_DABPLUS_MAX_AUS][DAB_MSC_CIFS_PER_FRAME * DAB_MSC_MAX_LOGICAL_FRAME_BYTES_PER_CIF];
} au_capture_t;

enum {
    AU_CAPTURE_QUEUE_CAPACITY = 8
};

typedef struct {
    au_capture_t items[AU_CAPTURE_QUEUE_CAPACITY];
    uint8_t count;
} au_capture_queue_t;

static const char *default_path(void) {
    const char *env = getenv("DAB_SYNC_FILE_REPLAY_PATH");

    if (env != 0 && env[0] != '\0') {
        return env;
    }
    return "/tmp/samples.bin";
}

static uint64_t monotonic_ms(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0u;
    }
    return ((uint64_t)ts.tv_sec * 1000u) + ((uint64_t)ts.tv_nsec / 1000000u);
}

static void sleep_ms(uint64_t ms) {
    struct timespec ts;

    ts.tv_sec = (time_t)(ms / 1000u);
    ts.tv_nsec = (long)((ms % 1000u) * 1000000u);
    (void)nanosleep(&ts, NULL);
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

static int env_u32_base0(const char *name, uint32_t *value_out) {
    const char *value = getenv(name);
    char *end = 0;
    unsigned long parsed;

    if (value_out == 0 || value == 0 || value[0] == '\0') {
        return -1;
    }
    parsed = strtoul(value, &end, 0);
    if (end == value || *end != '\0' || parsed > 0xfffffffful) {
        return -1;
    }
    *value_out = (uint32_t)parsed;
    return 0;
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

static int refine_symbol_fft_start_cp(const dab_prs_score_t *cp_score,
                                      const dab_sdr_block_view_t *block,
                                      uint64_t predicted_fft_start,
                                      int window_samples,
                                      uint64_t *best_fft_start_out,
                                      float *best_metric_out,
                                      float *best_cfo_hz_out) {
    int offset;
    int found = 0;
    float best_metric = -1.0f;
    float best_cfo_hz = 0.0f;
    uint64_t best_fft_start = predicted_fft_start;

    if (cp_score == 0 || block == 0 || best_fft_start_out == 0 ||
        best_metric_out == 0 || best_cfo_hz_out == 0 || window_samples < 0) {
        return -1;
    }

    for (offset = -window_samples; offset <= window_samples; ++offset) {
        uint64_t candidate_fft_start;
        float candidate_metric = 0.0f;
        float candidate_cfo_hz = 0.0f;

        if (offset < 0) {
            uint64_t abs_offset = (uint64_t)(-offset);
            if (predicted_fft_start < abs_offset) {
                continue;
            }
            candidate_fft_start = predicted_fft_start - abs_offset;
        } else {
            candidate_fft_start = predicted_fft_start + (uint64_t)offset;
        }

        if (dab_prs_score_cp_metric_i16(cp_score,
                                        block,
                                        candidate_fft_start,
                                        &candidate_metric,
                                        &candidate_cfo_hz) != 0) {
            continue;
        }
        if (!found || candidate_metric > best_metric) {
            found = 1;
            best_metric = candidate_metric;
            best_cfo_hz = candidate_cfo_hz;
            best_fft_start = candidate_fft_start;
        }
    }

    if (!found) {
        return 0;
    }

    *best_fft_start_out = best_fft_start;
    *best_metric_out = best_metric;
    *best_cfo_hz_out = best_cfo_hz;
    return 1;
}

static int history_init(history_buffer_t *history, uint32_t capacity_samples) {
    return dab_sample_history_init(history, capacity_samples);
}

static void history_destroy(history_buffer_t *history) {
    dab_sample_history_destroy(history);
}

static void history_append(history_buffer_t *history, const dab_sdr_block_view_t *block) {
    (void)dab_sample_history_append(history, block);
}

static dab_sdr_block_view_t history_view(const history_buffer_t *history) {
    return dab_sample_history_view(history);
}

static int history_contains_sample_range(const history_buffer_t *history,
                                         uint64_t start_sample,
                                         uint64_t end_sample) {
    return dab_sample_history_contains_range(history, start_sample, end_sample);
}

static int read_block(FILE *fp,
                      float **scratch,
                      uint32_t sample_count,
                      uint64_t sample_index,
                      dab_sdr_block_view_t *block,
                      int16_t **iq_buffer,
                      uint32_t *iq_capacity) {
    size_t read_count;
    uint32_t i;

    if (fp == 0 || scratch == 0 || block == 0 || iq_buffer == 0 || iq_capacity == 0 ||
        sample_count == 0u) {
        return -1;
    }

    if (*scratch == 0) {
        *scratch = (float *)malloc(sizeof(float) * 2u * sample_count);
        if (*scratch == 0) {
            return -1;
        }
    }
    if (*iq_capacity < sample_count) {
        int16_t *next = (int16_t *)realloc(*iq_buffer, sizeof(int16_t) * 2u * sample_count);
        if (next == 0) {
            return -1;
        }
        *iq_buffer = next;
        *iq_capacity = sample_count;
    }

    read_count = fread(*scratch, sizeof(float) * 2u, sample_count, fp);
    if (read_count == 0u) {
        return 0;
    }
    if (read_count != sample_count) {
        return 0;
    }

    for (i = 0u; i < sample_count; ++i) {
        (*iq_buffer)[2u * i] = (int16_t)(*scratch)[2u * i];
        (*iq_buffer)[2u * i + 1u] = (int16_t)(*scratch)[2u * i + 1u];
    }

    block->iq_interleaved = *iq_buffer;
    block->sample_count = sample_count;
    block->sample_index = sample_index;
    block->timestamp_ns = 0u;
    return 1;
}

static int find_primary_subchid_by_sid(const dab_fic_database_t *db, uint32_t sid, uint8_t *subchid_out) {
    uint16_t service_index;

    if (db == 0 || subchid_out == 0) {
        return -1;
    }

    for (service_index = 0u; service_index < db->service_count; ++service_index) {
        const dab_fic_service_t *service = &db->services[service_index];
        uint8_t component_index;

        if (service->sid != sid) {
            continue;
        }
        for (component_index = 0u; component_index < service->component_count; ++component_index) {
            const dab_fic_service_component_t *component = &service->components[component_index];

            if (component->primary != 0u && component->has_subchid != 0u) {
                *subchid_out = component->subchid;
                return 0;
            }
        }
    }

    return -1;
}

static int resolve_selected_stream(const dab_fic_database_t *db, selected_stream_t *selected) {
    if (selected == 0 || selected->valid == 0u) {
        return -1;
    }
    if (selected->resolved != 0u) {
        return 0;
    }
    if (find_primary_subchid_by_sid(db, selected->sid, &selected->subchid) != 0) {
        return -1;
    }
    selected->resolved = 1u;
    return 0;
}

static int build_ours_payload_for_selected(const dab_msc_state_t *msc,
                                           const selected_stream_t *selected,
                                           ours_stream_payload_t *payload) {
    const dab_msc_decoded_subchannel_t *subchannel;
    uint32_t cif_index;

    if (msc == 0 || selected == 0 || payload == 0 || selected->valid == 0u || selected->resolved == 0u) {
        return -1;
    }
    if (selected->subchid >= DAB_FIC_MAX_SUBCHANNELS) {
        return -1;
    }

    *payload = (ours_stream_payload_t){0};
    payload->subchid = selected->subchid;
    payload->sid = selected->sid;
    subchannel = &msc->decoded_subchannels[selected->subchid];
    if (subchannel->valid == 0u) {
        return -1;
    }

    payload->valid = 1u;
    payload->subchid = subchannel->subchid;
    for (cif_index = 0u; cif_index < DAB_MSC_CIFS_PER_FRAME; ++cif_index) {
        if (subchannel->cif_valid[cif_index] == 0u) {
            continue;
        }
        payload->byte_count[cif_index] = subchannel->byte_count[cif_index];
        payload->data[cif_index] = subchannel->data[cif_index];
    }

    return 0;
}

static int flatten_ours_payload(const ours_stream_payload_t *payload,
                                uint8_t *dst,
                                size_t dst_capacity,
                                size_t *bytes_written_out) {
    size_t offset = 0u;
    uint32_t cif_index;

    if (bytes_written_out != 0) {
        *bytes_written_out = 0u;
    }
    if (payload == 0 || dst == 0) {
        return -1;
    }

    for (cif_index = 0u; cif_index < DAB_MSC_CIFS_PER_FRAME; ++cif_index) {
        const size_t byte_count = payload->byte_count[cif_index];

        if (byte_count == 0u) {
            continue;
        }
        if (payload->data[cif_index] == 0 || offset + byte_count > dst_capacity) {
            return -1;
        }
        memcpy(dst + offset, payload->data[cif_index], byte_count);
        offset += byte_count;
    }

    if (bytes_written_out != 0) {
        *bytes_written_out = offset;
    }
    return 0;
}

static const dab_msc_decoded_component_t *find_selected_component_for_audio(const dab_msc_state_t *msc,
                                                                            const selected_stream_t *selected) {
    uint16_t component_index;
    const dab_msc_decoded_component_t *fallback = 0;

    if (msc == 0 || selected == 0 || selected->valid == 0u || selected->resolved == 0u) {
        return 0;
    }
    if (selected->sid != 0u) {
        return dab_msc_find_primary_decoded_component(msc, selected->sid);
    }

    for (component_index = 0u; component_index < msc->decoded_component_count; ++component_index) {
        const dab_msc_decoded_component_t *component = &msc->decoded_components[component_index];

        if (component->used == 0u || component->subchid != selected->subchid) {
            continue;
        }
        if (component->primary != 0u) {
            return component;
        }
        if (fallback == 0) {
            fallback = component;
        }
    }

    return fallback;
}

static int build_component_payload_from_ours(const ours_stream_payload_t *ours,
                                             const dab_msc_decoded_component_t *component,
                                             dab_msc_component_payload_t *payload) {
    uint32_t cif_index;

    if (ours == 0 || component == 0 || payload == 0 || ours->valid == 0u || component->used == 0u) {
        return -1;
    }

    *payload = (dab_msc_component_payload_t){0};
    payload->valid = 1u;
    payload->service_index = component->service_index;
    payload->component_index = component->component_index;
    payload->subchid = component->subchid;
    payload->primary = component->primary;
    payload->tmid = component->tmid;
    payload->sc_type = component->sc_type;
    payload->sid = component->sid;
    payload->kind = dab_msc_component_kind(component->tmid);
    payload->packet_addr = component->packet_addr;
    payload->has_packet_addr = component->has_packet_addr;
    payload->dg_flag = component->dg_flag;
    payload->caorg = component->caorg;
    payload->has_caorg = component->has_caorg;
    for (cif_index = 0u; cif_index < DAB_MSC_CIFS_PER_FRAME; ++cif_index) {
        if (ours->byte_count[cif_index] == 0u || ours->data[cif_index] == 0) {
            continue;
        }
        payload->cif_valid[cif_index] = 1u;
        payload->byte_count[cif_index] = ours->byte_count[cif_index];
        payload->data[cif_index] = ours->data[cif_index];
    }

    return 0;
}

static void capture_superframe_aus(const dab_msc_dabplus_superframe_t *superframe,
                                   au_capture_t *capture) {
    uint32_t i;

    if (capture == 0) {
        return;
    }
    *capture = (au_capture_t){0};
    if (superframe == 0 || superframe->valid == 0u || superframe->synchronized == 0u) {
        return;
    }

    capture->valid = 1u;
    capture->au_count = superframe->au_count;
    capture->sequence = superframe->generation;
    for (i = 0u; i < superframe->au_count && i < DAB_MSC_DABPLUS_MAX_AUS; ++i) {
        const uint32_t au_start = superframe->au_start[i];
        const uint32_t au_length = superframe->au_length[i];

        if (au_start + au_length > superframe->superframe_bytes) {
            capture->valid = 0u;
            return;
        }
        capture->au_length[i] = au_length;
        memcpy(capture->au_data[i], superframe->superframe_data + au_start, au_length);
    }
}

static int au_capture_equal(const au_capture_t *lhs, const au_capture_t *rhs) {
    uint32_t i;

    if (lhs == 0 || rhs == 0 || lhs->valid == 0u || rhs->valid == 0u || lhs->au_count != rhs->au_count) {
        return 0;
    }
    for (i = 0u; i < lhs->au_count; ++i) {
        if (lhs->au_length[i] != rhs->au_length[i]) {
            return 0;
        }
        if (memcmp(lhs->au_data[i], rhs->au_data[i], lhs->au_length[i]) != 0) {
            return 0;
        }
    }
    return 1;
}

static void au_capture_queue_drop_prefix(au_capture_queue_t *queue, uint8_t drop_count) {
    if (queue == 0 || drop_count == 0u || queue->count == 0u) {
        return;
    }
    if (drop_count >= queue->count) {
        queue->count = 0u;
        return;
    }
    memmove(&queue->items[0],
            &queue->items[drop_count],
            (size_t)(queue->count - drop_count) * sizeof(queue->items[0]));
    queue->count = (uint8_t)(queue->count - drop_count);
}

static void au_capture_queue_push(au_capture_queue_t *queue, const au_capture_t *capture) {
    if (queue == 0 || capture == 0 || capture->valid == 0u) {
        return;
    }
    if (queue->count != 0u && queue->items[queue->count - 1u].sequence == capture->sequence) {
        queue->items[queue->count - 1u] = *capture;
        return;
    }
    if (queue->count == AU_CAPTURE_QUEUE_CAPACITY) {
        au_capture_queue_drop_prefix(queue, 1u);
    }
    queue->items[queue->count++] = *capture;
}

static int au_capture_queue_find_alignment(const au_capture_queue_t *native_queue,
                                           const au_capture_queue_t *ref_queue,
                                           uint8_t *native_index_out,
                                           uint8_t *ref_index_out) {
    uint8_t native_index;

    if (native_index_out != 0) {
        *native_index_out = 0u;
    }
    if (ref_index_out != 0) {
        *ref_index_out = 0u;
    }
    if (native_queue == 0 || ref_queue == 0) {
        return 0;
    }

    for (native_index = 0u; native_index < native_queue->count; ++native_index) {
        uint8_t ref_index;

        for (ref_index = 0u; ref_index < ref_queue->count; ++ref_index) {
            if (!au_capture_equal(&native_queue->items[native_index], &ref_queue->items[ref_index])) {
                continue;
            }
            if (native_index_out != 0) {
                *native_index_out = native_index;
            }
            if (ref_index_out != 0) {
                *ref_index_out = ref_index;
            }
            return 1;
        }
    }

    return 0;
}

static int compare_au_capture(const au_capture_t *native_capture,
                              const au_capture_t *ref_capture,
                              uint32_t frame_index,
                              uint64_t frame_start_sample,
                              uint32_t sid,
                              uint8_t subchid) {
    uint32_t i;

    if (native_capture == 0 || ref_capture == 0 || native_capture->valid == 0u || ref_capture->valid == 0u) {
        return -1;
    }
    if (native_capture->au_count != ref_capture->au_count) {
        fprintf(stderr,
                "au-count-mismatch frame=%u frame_start=%llu sid=0x%08x subch=%u native=%u ref=%u\n",
                frame_index,
                (unsigned long long)frame_start_sample,
                sid,
                subchid,
                native_capture->au_count,
                ref_capture->au_count);
        return 1;
    }
    for (i = 0u; i < native_capture->au_count; ++i) {
        if (native_capture->au_length[i] != ref_capture->au_length[i]) {
            fprintf(stderr,
                    "au-length-mismatch frame=%u frame_start=%llu sid=0x%08x subch=%u au=%u native=%u ref=%u\n",
                    frame_index,
                    (unsigned long long)frame_start_sample,
                    sid,
                    subchid,
                    i,
                    native_capture->au_length[i],
                    ref_capture->au_length[i]);
            return 1;
        }
        if (memcmp(native_capture->au_data[i], ref_capture->au_data[i], native_capture->au_length[i]) != 0) {
            uint32_t j;

            for (j = 0u; j < native_capture->au_length[i]; ++j) {
                if (native_capture->au_data[i][j] != ref_capture->au_data[i][j]) {
                    fprintf(stderr,
                            "au-byte-mismatch frame=%u frame_start=%llu sid=0x%08x subch=%u au=%u offset=%u native=0x%02x ref=0x%02x\n",
                            frame_index,
                            (unsigned long long)frame_start_sample,
                            sid,
                            subchid,
                            i,
                            j,
                            native_capture->au_data[i][j],
                            ref_capture->au_data[i][j]);
                    break;
                }
            }
            return 1;
        }
    }

    printf("au-match frame=%u frame_start=%llu sid=0x%08x subch=%u au_count=%u\n",
           frame_index,
           (unsigned long long)frame_start_sample,
           sid,
           subchid,
           native_capture->au_count);
    return 0;
}

static int on_ref_dabplus_au_capture(void *user,
                                     unsigned long superframe_index,
                                     unsigned int au_index,
                                     unsigned int au_count,
                                     const unsigned char *data,
                                     unsigned long len) {
    au_capture_t *capture = (au_capture_t *)user;

    if (capture == 0 || data == 0 || au_count == 0u || au_count > DAB_MSC_DABPLUS_MAX_AUS ||
        au_index >= au_count || len > (unsigned long)(DAB_MSC_CIFS_PER_FRAME * DAB_MSC_MAX_LOGICAL_FRAME_BYTES_PER_CIF)) {
        return 1;
    }
    if (capture->sequence != (uint64_t)superframe_index) {
        *capture = (au_capture_t){0};
        capture->sequence = (uint64_t)superframe_index;
        capture->au_count = (uint8_t)au_count;
    }
    capture->au_length[au_index] = (uint32_t)len;
    memcpy(capture->au_data[au_index], data, len);
    if (au_index + 1u == au_count) {
        capture->valid = 1u;
    }
    return 1;
}

static void prepare_selected_compare_deinterleaver(const dab_msc_state_t *msc,
                                                   const dab_fic_database_t *db,
                                                   const selected_stream_t *selected,
                                                   dab_msc_deinterleaver_t *snapshot,
                                                   uint8_t *valid_out) {
    dab_msc_layout_t layout;
    const dab_msc_subchannel_t *subchannel;
    const dab_msc_subchannel_runtime_t *runtime;

    if (valid_out != 0) {
        *valid_out = 0u;
    }
    if (snapshot != 0) {
        memset(snapshot, 0, sizeof(*snapshot));
    }
    if (msc == 0 || db == 0 || selected == 0 || snapshot == 0 || valid_out == 0 ||
        selected->resolved == 0u || selected->subchid >= DAB_FIC_MAX_SUBCHANNELS) {
        return;
    }
    if (dab_msc_build_layout(&layout, db) != 0) {
        return;
    }

    subchannel = dab_msc_find_subchannel(&layout, selected->subchid);
    if (subchannel == 0 || subchannel->valid == 0u) {
        return;
    }

    runtime = &msc->runtimes[selected->subchid];
    if (runtime->valid != 0u &&
        runtime->deinterleaver != 0 &&
        runtime->subchid == subchannel->subchid &&
        runtime->config.profile == subchannel->profile &&
        runtime->config.protection_level == subchannel->protection_level &&
        runtime->config.bitrate_kbps == subchannel->bitrate_kbps &&
        runtime->config.size_cu == subchannel->size_cu) {
        *snapshot = *runtime->deinterleaver;
    }

    *valid_out = 1u;
}

static void copy_our_frame_into_ref_raw(SdrContext_t *ref_ctx, const int8_t *frame_softbits) {
    uint32_t i;

    if (ref_ctx == 0 || frame_softbits == 0) {
        return;
    }

    memset(&ref_ctx->tframe_raw, 0, sizeof(ref_ctx->tframe_raw));
    for (i = 0u; i < DAB_TFRAME_TOTAL_SOFTBITS; ++i) {
        ref_ctx->tframe_raw.data[i] = (bit_t)frame_softbits[i];
    }
}

static int compare_int8_stage(const char *stage,
                              const int8_t *ours,
                              const int *ref,
                              uint32_t count) {
    uint32_t i;

    for (i = 0u; i < count; ++i) {
        if ((int)ours[i] != ref[i]) {
            fprintf(stderr,
                    "%s-mismatch index=%u ours=%d ref=%d\n",
                    stage,
                    i,
                    (int)ours[i],
                    ref[i]);
            return 1;
        }
    }

    fprintf(stderr, "%s-match count=%u\n", stage, count);
    return 0;
}

static int compare_int16_stage(const char *stage,
                               const int16_t *ours,
                               const int *ref,
                               uint32_t count) {
    uint32_t i;

    for (i = 0u; i < count; ++i) {
        if ((int)ours[i] != ref[i]) {
            fprintf(stderr,
                    "%s-mismatch index=%u ours=%d ref=%d\n",
                    stage,
                    i,
                    (int)ours[i],
                    ref[i]);
            return 1;
        }
    }

    fprintf(stderr, "%s-match count=%u\n", stage, count);
    return 0;
}

static int compare_deinterleaver_state_stage(const char *stage,
                                             const dab_msc_deinterleaver_t *ours,
                                             const bit_t ref[][16],
                                             uint32_t conv_len) {
    uint32_t i;

    if (ours == 0 || ref == 0) {
        return 1;
    }

    for (i = 0u; i < conv_len; ++i) {
        uint32_t j;

        for (j = 0u; j < DAB_MSC_CONV_DEINTERLEAVER_DEPTH; ++j) {
            int8_t ours_value = 0;

            if (dab_msc_deinterleaver_get_history(ours, i, j, &ours_value) != 0 ||
                (int)ours_value != (int)ref[i][j]) {
                fprintf(stderr,
                        "%s-mismatch bit=%u delay=%u ours=%d ref=%d\n",
                        stage,
                        i,
                        j,
                        (int)ours_value,
                        (int)ref[i][j]);
                return 1;
            }
        }
    }

    fprintf(stderr, "%s-match count=%u\n", stage, conv_len * DAB_MSC_CONV_DEINTERLEAVER_DEPTH);
    return 0;
}

static void seed_ref_deinterleaver_state(dab_subch_runtime_t *ref_subch,
                                         const dab_msc_deinterleaver_t *ours,
                                         uint32_t conv_len) {
    uint32_t i;

    if (ref_subch == 0 || ours == 0) {
        return;
    }

    for (i = 0u; i < conv_len; ++i) {
        uint32_t j;

        for (j = 0u; j < DAB_MSC_CONV_DEINTERLEAVER_DEPTH; ++j) {
            int8_t value = 0;

            if (dab_msc_deinterleaver_get_history(ours, i, j, &value) == 0) {
                ref_subch->conv_reg_deintlv[i][j] = (bit_t)value;
            }
        }
    }
}

static int compare_u8_stage(const char *stage,
                            const uint8_t *ours,
                            const uint8_t *ref,
                            uint32_t count) {
    uint32_t i;

    for (i = 0u; i < count; ++i) {
        if (ours[i] != ref[i]) {
            fprintf(stderr,
                    "%s-mismatch index=%u ours=0x%02x ref=0x%02x\n",
                    stage,
                    i,
                    ours[i],
                    ref[i]);
            return 1;
        }
    }

    fprintf(stderr, "%s-match count=%u\n", stage, count);
    return 0;
}

static int compare_selected_stream(const ours_stream_payload_t *ours,
                                   const dab_msc_state_t *msc,
                                   const int8_t *msc_softbits,
                                   const dab_msc_deinterleaver_t *ours_debug_deinterleaver,
                                   SdrContext_t *ref_ctx,
                                   const selected_stream_t *selected,
                                   uint64_t frame_start_sample,
                                   uint32_t frame_index,
                                   int stop_on_match,
                                   int seed_ref_deint_state,
                                   int advance_only) {
    dab_subch_runtime_t *ref_subch;
    const dab_msc_subchannel_frame_t *subchannel_frame;
    const dab_msc_decoder_config_t *config;
    int8_t *ours_deinterleaved = 0;
    int16_t *ours_unpunctured = 0;
    uint8_t *ours_pre_descramble = 0;
    uint8_t *ours_post_descramble = 0;
    int8_t *ours_seq_deinterleaved = 0;
    int16_t *ours_seq_unpunctured = 0;
    uint8_t *ours_seq_pre_descramble = 0;
    uint8_t *ours_seq_post_descramble = 0;
    uint8_t *ours_runtime_bytes = 0;
    uint8_t *ours_runtime_max_bytes = 0;
    uint8_t *ref_cif_bytes = 0;
    size_t ref_cif_capacity = 0u;
    uint32_t cif_index;
    uint32_t expected_unpunctured_count;
    uint32_t byte_count;
    size_t ref_total_bytes = 0u;
    size_t ours_total_bytes = 0u;
    dab_msc_deinterleaver_t debug_deinterleaver;
    dab_msc_deinterleaver_t seq_deinterleaver;
    int8_t *diag_deinterleaved = 0;
    int rc = -1;

    if (ours == 0 || msc == 0 || msc_softbits == 0 || ours_debug_deinterleaver == 0 ||
        ref_ctx == 0 || selected == 0 || selected->resolved == 0u) {
        return 0;
    }
    if (selected->subchid >= DAB_FIC_MAX_SUBCHANNELS) {
        return -1;
    }

    ref_subch = &ref_ctx->tframe->msc_repo.subch[selected->subchid];
    if (ours->valid == 0u || ref_subch->valid == 0u) {
        return 0;
    }

    subchannel_frame = dab_msc_find_subchannel_frame(&msc->frame_view, selected->subchid);
    config = &msc->runtimes[selected->subchid].config;
    if (subchannel_frame == 0 || config->valid == 0u) {
        return -1;
    }

    expected_unpunctured_count = (config->framebits + 6u) * 4u;
    byte_count = config->framebits / 8u;
    ref_cif_capacity = (size_t)(ref_subch->rate / DAB_MSC_CIFS_PER_FRAME / 8u);
    ref_total_bytes = ref_cif_capacity * DAB_MSC_CIFS_PER_FRAME;
    for (cif_index = 0u; cif_index < DAB_MSC_CIFS_PER_FRAME; ++cif_index) {
        ours_total_bytes += ours->byte_count[cif_index];
    }
    if (ours_total_bytes != ref_total_bytes) {
        fprintf(stderr,
                "msc-size-mismatch frame=%u frame_start=%llu sid=0x%08x subch=%u ours=%zu ref=%zu\n",
                frame_index,
                (unsigned long long)frame_start_sample,
                ours->sid,
                selected->subchid,
                ours_total_bytes,
                ref_total_bytes);
        return 1;
    }

    ours_deinterleaved = (int8_t *)malloc(config->conv_len * DAB_MSC_CIFS_PER_FRAME);
    diag_deinterleaved = (int8_t *)malloc(config->conv_len);
    ours_unpunctured = (int16_t *)malloc(sizeof(*ours_unpunctured) * expected_unpunctured_count);
    ours_pre_descramble = (uint8_t *)malloc(byte_count);
    ours_post_descramble = (uint8_t *)malloc(byte_count);
    ours_seq_deinterleaved = (int8_t *)malloc(config->conv_len);
    ours_seq_unpunctured = (int16_t *)malloc(sizeof(*ours_seq_unpunctured) * expected_unpunctured_count);
    ours_seq_pre_descramble = (uint8_t *)malloc(byte_count);
    ours_seq_post_descramble = (uint8_t *)malloc(byte_count);
    ours_runtime_bytes = (uint8_t *)malloc(byte_count);
    ours_runtime_max_bytes = (uint8_t *)malloc(DAB_MSC_MAX_LOGICAL_FRAME_BYTES_PER_CIF);
    ref_cif_bytes = (uint8_t *)malloc(ref_cif_capacity);
    if (ours_deinterleaved == 0 || diag_deinterleaved == 0 || ours_unpunctured == 0 || ours_pre_descramble == 0 ||
        ours_post_descramble == 0 || ours_seq_deinterleaved == 0 || ours_seq_unpunctured == 0 ||
        ours_seq_pre_descramble == 0 || ours_seq_post_descramble == 0 || ours_runtime_bytes == 0 ||
        ours_runtime_max_bytes == 0 ||
        ref_cif_bytes == 0) {
        goto cleanup;
    }

    debug_deinterleaver = *ours_debug_deinterleaver;
    seq_deinterleaver = *ours_debug_deinterleaver;
    if (dab_msc_deinterleave_subchannel_frame(msc_softbits,
                                              DAB_TFRAME_MSC_SOFTBITS,
                                              subchannel_frame,
                                              config,
                                              &debug_deinterleaver,
                                              ours_deinterleaved,
                                              config->conv_len * DAB_MSC_CIFS_PER_FRAME) != 0) {
        goto cleanup;
    }

    unflatten_subch(ref_ctx->tframe, &ref_ctx->tframe_raw, ref_subch);
    if (seed_ref_deint_state != 0) {
        seed_ref_deinterleaver_state(ref_subch, ours_debug_deinterleaver, config->conv_len);
    }

    for (cif_index = 0u; cif_index < DAB_MSC_CIFS_PER_FRAME; ++cif_index) {
        uint32_t ours_byte_count = 0u;
        uint32_t ours_seq_byte_count = 0u;
        uint32_t i;
        const int8_t *ours_deinterleaved_cif = ours_deinterleaved + (cif_index * config->conv_len);
        const uint32_t msc_offset = subchannel_frame->cif_softbit_offset[cif_index];
        const int8_t *ours_deint_input = msc_softbits + msc_offset;
        dab_msc_deinterleaver_t runtime_like_state = seq_deinterleaver;

        (void)dab_msc_convolutional_deinterleave(&debug_deinterleaver,
                                                 ours_deint_input,
                                                 config->conv_len,
                                                 diag_deinterleaved,
                                                 config->conv_len);
        if (dab_msc_convolutional_deinterleave(&seq_deinterleaver,
                                               ours_deint_input,
                                               config->conv_len,
                                               ours_seq_deinterleaved,
                                               config->conv_len) != 0) {
            goto cleanup;
        }
        if (dab_msc_decode_deinterleaved_cif_debug(ours_seq_deinterleaved,
                                                   config->conv_len,
                                                   config,
                                                   ours_seq_unpunctured,
                                                   expected_unpunctured_count,
                                                   ours_seq_pre_descramble,
                                                   byte_count,
                                                   ours_seq_post_descramble,
                                                   byte_count,
                                                   &ours_seq_byte_count) != 0) {
            goto cleanup;
        }
        if (dab_msc_decode_subchannel_cif(msc_softbits,
                                          DAB_TFRAME_MSC_SOFTBITS,
                                          subchannel_frame,
                                          config,
                                          &runtime_like_state,
                                          cif_index,
                                          ours_runtime_bytes,
                                          byte_count) != 0) {
            goto cleanup;
        }
        if (dab_msc_decode_subchannel_cif(msc_softbits,
                                          DAB_TFRAME_MSC_SOFTBITS,
                                          subchannel_frame,
                                          config,
                                          &runtime_like_state,
                                          cif_index,
                                          ours_runtime_max_bytes,
                                          DAB_MSC_MAX_LOGICAL_FRAME_BYTES_PER_CIF) != 0) {
            goto cleanup;
        }

        if (dab_msc_decode_deinterleaved_cif_debug(ours_deinterleaved_cif,
                                                   config->conv_len,
                                                   config,
                                                   ours_unpunctured,
                                                   expected_unpunctured_count,
                                                   ours_pre_descramble,
                                                   byte_count,
                                                   ours_post_descramble,
                                                   byte_count,
                                                   &ours_byte_count) != 0) {
            goto cleanup;
        }
        if (subch_read_cif(ref_subch, ref_cif_bytes, (int)cif_index) != 0) {
            goto cleanup;
        }
        if (advance_only != 0) {
            continue;
        }
        if (ours_byte_count != byte_count || ours_seq_byte_count != byte_count ||
            ours->byte_count[cif_index] != byte_count ||
            ref_cif_capacity != byte_count) {
            fprintf(stderr,
                    "msc-cif-size-mismatch frame=%u frame_start=%llu sid=0x%08x subch=%u cif=%u ours_debug=%u ours_seq=%u ours_payload=%u ref=%u\n",
                    frame_index,
                    (unsigned long long)frame_start_sample,
                    ours->sid,
                    selected->subchid,
                    cif_index,
                    ours_byte_count,
                    ours_seq_byte_count,
                    ours->byte_count[cif_index],
                    (unsigned)ref_cif_capacity);
            rc = 1;
            goto cleanup;
        }

        for (i = 0u; i < byte_count; ++i) {
            const uint8_t ours_byte = ours->data[cif_index][i];
            const uint8_t ref_byte = ref_cif_bytes[i];

            if (ours_byte != ref_byte) {
                fprintf(stderr,
                        "msc-mismatch frame=%u frame_start=%llu sid=0x%08x subch=%u cif=%u cif_offset=%u total_offset=%zu ours=0x%02x ref=0x%02x\n",
                        frame_index,
                        (unsigned long long)frame_start_sample,
                        ours->sid,
                        selected->subchid,
                        cif_index,
                        i,
                        ((size_t)cif_index * byte_count) + i,
                        ours_byte,
                        ref_byte);
                (void)compare_int8_stage("msc-stage-deinterleaved",
                                         ours_deinterleaved_cif,
                                         ref_subch->conv_dec,
                                         config->conv_len);
                (void)compare_int8_stage("msc-stage-wholeframe-vs-seq-deinterleaved",
                                         ours_deinterleaved_cif,
                                         ours_seq_deinterleaved,
                                         config->conv_len);
                (void)compare_int8_stage("msc-stage-deint-input",
                                         ours_deint_input,
                                         ref_subch->conv_deintlv_cif[cif_index],
                                         config->conv_len);
                (void)compare_deinterleaver_state_stage("msc-stage-deint-state",
                                                        &runtime_like_state,
                                                        ref_subch->conv_reg_deintlv,
                                                        config->conv_len);
                (void)compare_int8_stage("msc-stage-deint-one-cif",
                                         diag_deinterleaved,
                                         ref_subch->conv_dec,
                                         config->conv_len);
                (void)compare_int16_stage("msc-stage-unpunctured",
                                          ours_unpunctured,
                                          ref_subch->conv_up,
                                          expected_unpunctured_count);
                (void)compare_u8_stage("msc-stage-pre-descramble",
                                       ours_pre_descramble,
                                       ref_subch->viterbi,
                                       byte_count);
                (void)compare_u8_stage("msc-stage-ours-stored-vs-debug-post-descramble",
                                       ours->data[cif_index],
                                       ours_post_descramble,
                                       byte_count);
                (void)compare_u8_stage("msc-stage-ours-stored-vs-runtime-func",
                                       ours->data[cif_index],
                                       ours_runtime_bytes,
                                       byte_count);
                (void)compare_u8_stage("msc-stage-ours-stored-vs-runtime-func-maxbuf",
                                       ours->data[cif_index],
                                       ours_runtime_max_bytes,
                                       byte_count);
                (void)compare_u8_stage("msc-stage-runtime-func-vs-seq-debug-post-descramble",
                                       ours_runtime_bytes,
                                       ours_seq_post_descramble,
                                       byte_count);
                (void)compare_u8_stage("msc-stage-post-descramble",
                                       ours_post_descramble,
                                       ref_cif_bytes,
                                       byte_count);
                rc = 1;
                goto cleanup;
            }
        }
    }

    if (stop_on_match != 0) {
        printf("msc-match frame=%u frame_start=%llu sid=0x%08x subch=%u bytes=%zu\n",
               frame_index,
               (unsigned long long)frame_start_sample,
               ours->sid,
               selected->subchid,
                ref_total_bytes);
        rc = 2;
        goto cleanup;
    }

    printf("msc-match frame=%u frame_start=%llu sid=0x%08x subch=%u bytes=%zu\n",
           frame_index,
           (unsigned long long)frame_start_sample,
           ours->sid,
           selected->subchid,
           ref_total_bytes);
    rc = 0;

cleanup:
    free(ref_cif_bytes);
    free(ours_runtime_max_bytes);
    free(ours_runtime_bytes);
    free(ours_seq_post_descramble);
    free(ours_seq_pre_descramble);
    free(ours_seq_unpunctured);
    free(ours_seq_deinterleaved);
    free(ours_post_descramble);
    free(ours_pre_descramble);
    free(ours_unpunctured);
    free(diag_deinterleaved);
    free(ours_deinterleaved);
    return rc;
}

int main(int argc, char **argv) {
    const char *path = argc > 1 ? argv[1] : default_path();
    FILE *fp = 0;
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
    dab_sdr_block_view_t src_block = {0};
    dab_null_event_t event = {0};
    const dab_simd_ops_t *simd_ops = dab_simd_get_ops();
    float *fft_i = 0;
    float *fft_q = 0;
    float *active_i = 0;
    float *active_q = 0;
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
    float *sample_scratch = 0;
    int16_t *iq_block = 0;
    uint32_t iq_capacity = 0u;
    uint32_t block_samples = env_u32("DAB_SYNC_FILE_REPLAY_BLOCK_SAMPLES", 2552u);
    uint64_t sample_index = 0u;
    uint64_t blocks_read = 0u;
    uint64_t last_predicted_fft_start = 0u;
    float ref_threshold = env_f32("DAB_SYNC_PROBE_REF_SCORE_THRESHOLD", 0.10f);
    int fft_start_bias_samples = env_i32("DAB_SYNC_PROBE_FFT_START_BIAS_SAMPLES", 0);
    int use_symbol_cp_cfo = env_i32("DAB_SYNC_PROBE_USE_SYMBOL_CP_CFO", 0);
    int symbol_cp_timing_window = env_i32("DAB_SYNC_PROBE_SYMBOL_CP_TIMING_WINDOW", 0);
    int skip_initial_data_symbols = env_i32("DAB_SYNC_PROBE_SKIP_INITIAL_DATA_SYMBOLS", 0);
    int data_symbol_start_offset = env_i32("DAB_SYNC_PROBE_DATA_SYMBOL_START_OFFSET", 0);
    int apply_data_cpe_correction = env_i32("DAB_SYNC_PROBE_APPLY_DATA_CPE_CORRECTION", 0);
    float prs_fine_timing_limit = env_f32("DAB_SYNC_PROBE_PRS_FINE_TIMING_LIMIT", 32.0f);
    uint32_t sample_rate = env_u32("DAB_SYNC_PROBE_SAMPLE_RATE", 2048000u);
    uint32_t selected_sid = 0u;
    uint32_t selected_subchid_u32 = 0u;
    int selected_sid_configured = env_u32_base0("DAB_MSC_REF_COMPARE_SID", &selected_sid) == 0;
    int selected_subchid_configured = env_u32_base0("DAB_MSC_REF_COMPARE_SUBCHID", &selected_subchid_u32) == 0;
    int compare_enabled = env_i32("DAB_MSC_REF_COMPARE_COMPARE", 1);
    int stop_on_match = env_i32("DAB_MSC_REF_COMPARE_STOP_ON_MATCH", 0);
    int play_audio = env_i32("DAB_MSC_REF_COMPARE_PLAY_AUDIO", 0);
    int play_audio_from_ours = env_i32("DAB_MSC_REF_COMPARE_PLAY_AUDIO_FROM_OURS", 0);
    int play_audio_superframe = env_i32("DAB_MSC_REF_COMPARE_PLAY_AUDIO_SUPERFRAME", 0);
    int compare_au = env_i32("DAB_MSC_REF_COMPARE_COMPARE_AU", 0);
    unsigned int warmup_frames = env_u32("DAB_MSC_REF_COMPARE_WARMUP_FRAMES", 0u);
    int seed_ref_deint_state = env_i32("DAB_MSC_REF_COMPARE_SEED_REF_DEINT_STATE", 0);
    unsigned int frame_limit = env_u32("DAB_MSC_REF_COMPARE_FRAME_LIMIT", 0u);
    int audio_realtime = env_i32("DAB_MSC_REF_COMPARE_AUDIO_REALTIME", play_audio != 0 ? 1 : 0);
    unsigned int audio_drain_ms = env_u32("DAB_MSC_REF_COMPARE_AUDIO_DRAIN_MS", play_audio != 0 ? 1000u : 0u);
    int audio_debug_enabled = env_i32("DAB_MSC_REF_COMPARE_AUDIO_DEBUG", 0);
    selected_stream_t selected = {0};
    sdr_context_t ref_handle = 0;
    SdrContext_t *ref_ctx = 0;
    dab_msc_deinterleaver_t selected_compare_deinterleaver = {0};
    uint8_t selected_compare_deinterleaver_valid = 0u;
    uint32_t compared_frames = 0u;
    uint64_t replay_wallclock_start_ms = 0u;
    uint64_t replay_sample_origin = 0u;
    int replay_clock_started = 0;
    int mismatch_found = 0;
    int au_mismatch_found = 0;
    int rc = 1;
#if defined(DAB_HAVE_AUDIO_PLAYBACK)
    dab_audio_playback_t audio = {0};
#endif
    dab_ref_backend_hooks_t ref_au_hooks = {0};
    au_capture_t ref_au_capture = {0};
    au_capture_t native_au_capture = {0};
    au_capture_queue_t ref_au_queue = {0};
    au_capture_queue_t native_au_queue = {0};
    int au_compare_started = 0;
    uint32_t au_native_captures = 0u;
    uint32_t au_ref_captures = 0u;
    uint32_t au_matched_sets = 0u;
    uint32_t au_alignment_events = 0u;
    uint32_t au_alignment_drop_native = 0u;
    uint32_t au_alignment_drop_ref = 0u;
    dab_msc_dabplus_debug_t final_native_debug = {0};
    int final_native_debug_valid = 0;

    if (!selected_sid_configured && !selected_subchid_configured) {
        fprintf(stderr, "set DAB_MSC_REF_COMPARE_SID or DAB_MSC_REF_COMPARE_SUBCHID\n");
        return 2;
    }
    if (selected_sid_configured && selected_subchid_configured) {
        fprintf(stderr, "set only one of DAB_MSC_REF_COMPARE_SID or DAB_MSC_REF_COMPARE_SUBCHID\n");
        return 2;
    }
    if ((play_audio_from_ours != 0) + (play_audio_superframe != 0) > 1) {
        fprintf(stderr,
                "set at most one of DAB_MSC_REF_COMPARE_PLAY_AUDIO_FROM_OURS or DAB_MSC_REF_COMPARE_PLAY_AUDIO_SUPERFRAME\n");
        return 2;
    }
#if !defined(DAB_HAVE_AUDIO_PLAYBACK)
    if (play_audio_superframe != 0) {
        fprintf(stderr, "DAB_MSC_REF_COMPARE_PLAY_AUDIO_SUPERFRAME requires native audio playback support at build time\n");
        return 2;
    }
#endif

    if (selected_sid_configured) {
        selected.valid = 1u;
        selected.sid = selected_sid;
    } else if (selected_subchid_u32 < DAB_FIC_MAX_SUBCHANNELS) {
        selected.valid = 1u;
        selected.subchid = (uint8_t)selected_subchid_u32;
        selected.resolved = 1u;
    } else {
        fprintf(stderr, "invalid DAB_MSC_REF_COMPARE_SUBCHID=%u\n", selected_subchid_u32);
        return 2;
    }

#if defined(DAB_HAVE_AUDIO_PLAYBACK)
    audio.debug_enabled = (uint8_t)(audio_debug_enabled != 0);
#endif
    ref_au_hooks.magic = DAB_REF_BACKEND_HOOKS_MAGIC;
    ref_au_hooks.on_dabplus_au = on_ref_dabplus_au_capture;
    ref_au_hooks.user = &ref_au_capture;

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
    score_config.sample_rate = sample_rate;
    score_config.guard_samples = acquire_config.guard_samples;
    score_config.fft_samples = acquire_config.fft_samples;
    if (dab_prs_score_init(&cp_score, &score_config) != 0) {
        fprintf(stderr, "prs score init failed\n");
        return 2;
    }

    dab_prs_fft_default_config(&fft_config);
    fft_config.fft_samples = acquire_config.fft_samples;
    fft_config.sample_rate = sample_rate;
    if (dab_prs_fft_init(&fft, &fft_config) != 0) {
        fprintf(stderr, "prs fft init failed\n");
        return 2;
    }

    dab_sync_default_config(&sync_config);
    sync_config.acquire_timing_window_samples = acquire_config.acquire_window_samples;
    sync_config.track_timing_window_samples =
        env_u32("DAB_SYNC_PROBE_TRACK_WINDOW_SAMPLES", sync_config.track_timing_window_samples);
    sync_config.acquire_prs_score_threshold =
        env_f32("DAB_SYNC_PROBE_ACQUIRE_PRS_SCORE_THRESHOLD", ref_threshold);
    sync_config.track_prs_score_threshold =
        env_f32("DAB_SYNC_PROBE_TRACK_PRS_SCORE_THRESHOLD", ref_threshold * 0.8f);
    sync_config.cfo_pi_proportional_gain =
        env_f32("DAB_SYNC_PROBE_CFO_PI_KP", sync_config.cfo_pi_proportional_gain);
    sync_config.cfo_pi_integral_gain =
        env_f32("DAB_SYNC_PROBE_CFO_PI_KI", sync_config.cfo_pi_integral_gain);
    sync_config.cfo_pi_integral_limit_hz =
        env_f32("DAB_SYNC_PROBE_CFO_PI_I_LIMIT_HZ", sync_config.cfo_pi_integral_limit_hz);
    if (dab_sync_init(&sync, &sync_config) != 0) {
        fprintf(stderr, "sync init failed\n");
        return 2;
    }
    if (dab_fic_init(&fic) != 0) {
        fprintf(stderr, "fic/msc init failed\n");
        return 2;
    }
    if (dab_msc_init(&msc) != 0) {
        fprintf(stderr, "fic/msc init failed\n");
        dab_fic_destroy(&fic);
        return 2;
    }
    dab_ofdm_default_config(&ofdm_config);
    if (dab_ofdm_init(&ofdm, &ofdm_config) != 0) {
        fprintf(stderr, "ofdm init failed\n");
        dab_fic_destroy(&fic);
        return 2;
    }

    fft_i = (float *)calloc(acquire_config.fft_samples, sizeof(*fft_i));
    fft_q = (float *)calloc(acquire_config.fft_samples, sizeof(*fft_q));
    active_i = (float *)calloc(DAB_MODE_I_ACTIVE_CARRIERS, sizeof(*active_i));
    active_q = (float *)calloc(DAB_MODE_I_ACTIVE_CARRIERS, sizeof(*active_q));
    prev_active_i = (float *)calloc(DAB_MODE_I_ACTIVE_CARRIERS, sizeof(*prev_active_i));
    prev_active_q = (float *)calloc(DAB_MODE_I_ACTIVE_CARRIERS, sizeof(*prev_active_q));
    diff_i = (float *)calloc(DAB_MODE_I_DATA_CARRIERS, sizeof(*diff_i));
    diff_q = (float *)calloc(DAB_MODE_I_DATA_CARRIERS, sizeof(*diff_q));
    deintl_i = (float *)calloc(DAB_MODE_I_DATA_CARRIERS, sizeof(*deintl_i));
    deintl_q = (float *)calloc(DAB_MODE_I_DATA_CARRIERS, sizeof(*deintl_q));
    soft_a = (int8_t *)calloc(DAB_MODE_I_DATA_CARRIERS, sizeof(*soft_a));
    soft_b = (int8_t *)calloc(DAB_MODE_I_DATA_CARRIERS, sizeof(*soft_b));
    fic_softbits = (int8_t *)calloc(DAB_TFRAME_FIC_SOFTBITS, sizeof(*fic_softbits));
    msc_softbits = (int8_t *)calloc(DAB_TFRAME_MSC_SOFTBITS, sizeof(*msc_softbits));
    frame_softbits = (int8_t *)calloc(DAB_TFRAME_TOTAL_SOFTBITS, sizeof(*frame_softbits));
    if (fft_i == 0 || fft_q == 0 || active_i == 0 || active_q == 0 ||
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

    fp = fopen(path, "rb");
    if (fp == 0) {
        perror(path);
        goto cleanup;
    }

    ref_handle = sdrplay_api_init(NULL);
    if (ref_handle == 0) {
        fprintf(stderr, "reference init failed\n");
        goto cleanup;
    }
    ref_ctx = (SdrContext_t *)ref_handle;

    printf("msc-ref-compare start path=%s sample_rate=%u block_samples=%u simd=%s fft=%s threshold=%.3f",
           path,
           sample_rate,
           block_samples,
           simd_ops != 0 ? simd_ops->name : "unknown",
           fft.backend_name != 0 ? fft.backend_name : "unknown",
           ref_threshold);
    if (selected_sid_configured) {
        printf(" sid=0x%08x compare=%d compare_au=%d play_audio=%d play_audio_from_ours=%d play_audio_superframe=%d warmup=%u seed_ref_state=%d realtime=%d drain_ms=%u\n",
               selected_sid,
               compare_enabled,
               compare_au,
               play_audio,
               play_audio_from_ours,
               play_audio_superframe,
               warmup_frames,
               seed_ref_deint_state,
               audio_realtime,
               audio_drain_ms);
    } else {
        printf(" subch=%u compare=%d compare_au=%d play_audio=%d play_audio_from_ours=%d play_audio_superframe=%d warmup=%u seed_ref_state=%d realtime=%d drain_ms=%u\n",
               (unsigned)selected.subchid,
               compare_enabled,
               compare_au,
               play_audio,
               play_audio_from_ours,
               play_audio_superframe,
               warmup_frames,
               seed_ref_deint_state,
               audio_realtime,
               audio_drain_ms);
    }

    while (1) {
        dab_sdr_block_view_t hist_view;
        int read_rc;

        read_rc = read_block(fp, &sample_scratch, block_samples, sample_index, &src_block, &iq_block, &iq_capacity);
        if (read_rc < 0) {
            fprintf(stderr, "read failed\n");
            goto cleanup;
        }
        if (read_rc == 0) {
            break;
        }
        sample_index += src_block.sample_count;
        blocks_read++;
        history_append(&history, &src_block);

        if (dab_null_detect_process_i16(&detector, &src_block, &event) < 0) {
            fprintf(stderr, "null detector failed at block %llu\n", (unsigned long long)blocks_read);
            goto cleanup;
        }

        if (event.length != 0u && !sync.state.locked) {
            if (dab_prs_candidate_from_null(&acquire, &event, &pending.candidate) == 0) {
                pending.pending = 1;
                pending.predicted = 0;
                pending.event = event;
            }
        }

        if (!pending.pending && sync.state.locked) {
            dab_prs_candidate_t predicted_candidate = {0};

            predicted_candidate.prs_fft_start_sample = dab_sync_predict_next_frame_start(&sync);
            predicted_candidate.prs_symbol_start_sample =
                predicted_candidate.prs_fft_start_sample - acquire.config.guard_samples;
            predicted_candidate.null_start_sample =
                predicted_candidate.prs_symbol_start_sample - acquire.config.null_samples;
            predicted_candidate.timing_search_start_offset = sync.config.track_timing_window_samples / 2u;
            predicted_candidate.timing_search_length = sync.config.track_timing_window_samples;
            if (predicted_candidate.prs_fft_start_sample != last_predicted_fft_start) {
                pending.pending = 1;
                pending.predicted = 1;
                pending.candidate = predicted_candidate;
                pending.event = (dab_null_event_t){0};
                last_predicted_fft_start = predicted_candidate.prs_fft_start_sample;
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
            int coarse_rc = dab_prs_score_search_i16(&cp_score, &hist_view, &pending.candidate, &coarse);

            if (coarse_rc == 1) {
                float cfo_for_fft = coarse.coarse_freq_error_hz;
                uint64_t prs_fft_start_sample;

                if (sync.state.locked && sync.state.bad_frames == 0u) {
                    cfo_for_fft = sync.state.coarse_freq_error_hz;
                }
                if ((int64_t)coarse.prs_fft_start_sample + (int64_t)fft_start_bias_samples < 0) {
                    pending.pending = 0;
                    pending.predicted = 0;
                    continue;
                }
                prs_fft_start_sample =
                    (uint64_t)((int64_t)coarse.prs_fft_start_sample + (int64_t)fft_start_bias_samples);
                if (dab_prs_score_symbol_i16_mode_i_diff_cfo(&fft,
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

                prs_fft_start_sample = apply_prs_fine_timing(prs_fft_start_sample,
                                                             -ref_score.fine_timing_error_samples,
                                                             prs_fine_timing_limit,
                                                             0);
                if (ref_score.carrier_offset_bins != 0) {
                    (void)dab_prs_fft_apply_bin_shift(fft_i,
                                                      fft_q,
                                                      acquire_config.fft_samples,
                                                      -ref_score.carrier_offset_bins);
                }

                if (sync.state.locked) {
                    (void)dab_sync_on_prs_track(&sync,
                                                prs_fft_start_sample,
                                                ref_score.score,
                                                coarse.coarse_freq_error_hz,
                                                ref_score.carrier_offset_bins);
                } else {
                    dab_sync_on_null_candidate(&sync, &pending.event);
                    (void)dab_sync_on_prs_acquire(&sync,
                                                  prs_fft_start_sample,
                                                  ref_score.score,
                                                  coarse.coarse_freq_error_hz,
                                                  ref_score.carrier_offset_bins);
                }

                if (!sync.state.locked) {
                    last_predicted_fft_start = 0u;
                    dab_ofdm_reset(&ofdm);
                    dab_tframe_reset(&tframe);
                }

                if (ref_score.score >= ref_threshold) {
                    dab_ofdm_buffers_t ofdm_buffers;
                    dab_ofdm_result_t ofdm_result;
                    uint64_t data_fft_start;
                            dab_tframe_result_t tframe_result;
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

                    for (symbol_idx = 0u; symbol_idx < DAB_TFRAME_TOTAL_SYMBOLS; ++symbol_idx) {
                        int logical_symbol_index = (int)symbol_idx + 1 + data_symbol_start_offset;
                        float symbol_cfo_hz = cfo_for_fft;

                        if (logical_symbol_index <= 0) {
                            continue;
                        }
                        data_fft_start = prs_fft_start_sample +
                                         ((uint64_t)logical_symbol_index * acquire_config.symbol_samples);
                        if (symbol_cp_timing_window > 0) {
                            float cp_metric = 0.0f;
                            float cp_cfo_hz = 0.0f;
                            uint64_t refined_fft_start = data_fft_start;
                            int refine_rc = refine_symbol_fft_start_cp(&cp_score,
                                                                       &hist_view,
                                                                       data_fft_start,
                                                                       symbol_cp_timing_window,
                                                                       &refined_fft_start,
                                                                       &cp_metric,
                                                                       &cp_cfo_hz);
                            if (refine_rc > 0) {
                                data_fft_start = refined_fft_start;
                                if (use_symbol_cp_cfo) {
                                    symbol_cfo_hz = cp_cfo_hz;
                                }
                            }
                        }
                        if (!history_contains_sample_range(&history,
                                                           data_fft_start,
                                                           data_fft_start + acquire_config.fft_samples)) {
                            break;
                        }
                        if (use_symbol_cp_cfo) {
                            float cp_metric = 0.0f;
                            float cp_cfo_hz = 0.0f;
                            if (dab_prs_score_cp_metric_i16(&cp_score,
                                                            &hist_view,
                                                            data_fft_start,
                                                            &cp_metric,
                                                            &cp_cfo_hz) == 0) {
                                symbol_cfo_hz = cp_cfo_hz;
                            }
                        }
                        if (dab_prs_fft_execute_i16_cfo(&fft,
                                                        &hist_view,
                                                        data_fft_start,
                                                        symbol_cfo_hz,
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
                        if (dab_ofdm_process_mode_i(&ofdm,
                                                    fft_i,
                                                    fft_q,
                                                    data_fft_start,
                                                    DAB_OFDM_SYMBOL_DATA,
                                                    &ofdm_buffers,
                                                    &ofdm_result) != 0) {
                            break;
                        }
                        if (!ofdm_result.have_diff) {
                            continue;
                        }
                        if (ofdm_result.have_softbits) {
                            const float data_cpe_deg =
                                decision_directed_phase_error_deg(deintl_i,
                                                                  deintl_q,
                                                                  DAB_MODE_I_DATA_CARRIERS);
                            if (apply_data_cpe_correction) {
                                rotate_complex_f32(deintl_i,
                                                   deintl_q,
                                                   DAB_MODE_I_DATA_CARRIERS,
                                                   -data_cpe_deg);
                                if (dab_ofdm_qpsk_soft_decode_mode_i(deintl_i,
                                                                     deintl_q,
                                                                     soft_a,
                                                                     soft_b) != 0) {
                                    break;
                                }
                            }
                        }
                        if ((int)symbol_idx < skip_initial_data_symbols) {
                            continue;
                        }
                        if (ofdm_result.have_softbits &&
                            dab_tframe_push_symbol(&tframe,
                                                   data_fft_start,
                                                   soft_a,
                                                   soft_b,
                                                   &tframe_result) == 0 &&
                            tframe_result.frame_complete) {
                            ours_stream_payload_t ours_payload = {0};
                            if (dab_fic_process_frame(&fic,
                                                      tframe_result.frame_start_sample,
                                                      fic_softbits,
                                                      DAB_TFRAME_FIC_SOFTBITS) != 0) {
                                continue;
                            }
                            if (resolve_selected_stream(&fic.repo.current, &selected) == 0) {
                                prepare_selected_compare_deinterleaver(&msc,
                                                                       &fic.repo.current,
                                                                       &selected,
                                                                       &selected_compare_deinterleaver,
                                                                       &selected_compare_deinterleaver_valid);
                            } else {
                                selected_compare_deinterleaver_valid = 0u;
                            }
                            if (dab_msc_process_frame_with_fic(&msc,
                                                               tframe_result.frame_start_sample,
                                                               msc_softbits,
                                                               DAB_TFRAME_MSC_SOFTBITS,
                                                               &fic.repo.current) != 0) {
                                continue;
                            }

                            if (resolve_selected_stream(&fic.repo.current, &selected) == 0 &&
                                build_ours_payload_for_selected(&msc, &selected, &ours_payload) == 0) {
                                int compare_rc = 0;
                                const dab_msc_decoded_component_t *audio_component = 0;
                                dab_msc_dabplus_superframe_t superframe = {0};
                                int have_native_superframe = 0;

                                if (play_audio != 0 && audio_realtime != 0) {
                                    if (!replay_clock_started) {
                                        replay_wallclock_start_ms = monotonic_ms();
                                        replay_sample_origin = tframe_result.frame_start_sample;
                                        replay_clock_started = 1;
                                    } else {
                                        uint64_t elapsed_samples =
                                            tframe_result.frame_start_sample - replay_sample_origin;
                                        uint64_t target_ms =
                                            replay_wallclock_start_ms + ((elapsed_samples * 1000u) / sample_rate);
                                        uint64_t now_ms = monotonic_ms();

                                        if (target_ms > now_ms) {
                                            sleep_ms(target_ms - now_ms);
                                        }
                                    }
                                }

                                copy_our_frame_into_ref_raw(ref_ctx, frame_softbits);
                                unflatten_tframe(ref_ctx->tframe, &ref_ctx->tframe_raw);

                                compared_frames++;
                                if (compare_enabled != 0) {
                                    if (compared_frames <= warmup_frames) {
                                        printf("msc-warmup frame=%u frame_start=%llu sid=0x%08x subch=%u\n",
                                               compared_frames,
                                               (unsigned long long)tframe_result.frame_start_sample,
                                               ours_payload.sid,
                                               selected.subchid);
                                        if (selected_compare_deinterleaver_valid != 0u) {
                                            (void)compare_selected_stream(&ours_payload,
                                                                         &msc,
                                                                         msc_softbits,
                                                                         &selected_compare_deinterleaver,
                                                                         ref_ctx,
                                                                         &selected,
                                                                         tframe_result.frame_start_sample,
                                                                         compared_frames,
                                                                         0,
                                                                         seed_ref_deint_state,
                                                                         1);
                                        }
                                        continue;
                                    }
                                    if (selected_compare_deinterleaver_valid == 0u) {
                                        continue;
                                    }
                                    compare_rc = compare_selected_stream(&ours_payload,
                                                                         &msc,
                                                                         msc_softbits,
                                                                         &selected_compare_deinterleaver,
                                                                         ref_ctx,
                                                                         &selected,
                                                                         tframe_result.frame_start_sample,
                                                                         compared_frames,
                                                                         stop_on_match,
                                                                         seed_ref_deint_state,
                                                                         0);
                                    if (compare_rc < 0) {
                                        goto cleanup;
                                    }
                                }
                                {
                                    const uint64_t native_sequence_before = native_au_capture.sequence;
                                    const uint8_t native_valid_before = native_au_capture.valid;
                                    const uint64_t ref_sequence_before = ref_au_capture.sequence;
                                    const uint8_t ref_valid_before = ref_au_capture.valid;
                                    uint8_t native_align_index = 0u;
                                    uint8_t ref_align_index = 0u;
                                    int native_fresh = 0;
                                    int ref_fresh = 0;

                                    if (compare_au != 0 || play_audio_superframe != 0) {
                                        audio_component = find_selected_component_for_audio(&msc, &selected);
                                        if (audio_component != 0) {
                                            int native_superframe_rc;
                                            int native_debug_rc;

                                            if (selected.sid != 0u) {
                                                native_superframe_rc =
                                                    dab_msc_get_primary_dabplus_superframe(&msc, selected.sid, &superframe);
                                                native_debug_rc =
                                                    dab_msc_get_primary_dabplus_debug(&msc, selected.sid, &final_native_debug);
                                            } else {
                                                native_superframe_rc =
                                                    dab_msc_get_dabplus_superframe(&msc, audio_component, &superframe);
                                                native_debug_rc =
                                                    dab_msc_get_dabplus_debug(&msc, audio_component, &final_native_debug);
                                            }
                                            if (native_debug_rc == 0 && final_native_debug.valid != 0u) {
                                                final_native_debug_valid = 1;
                                            }
                                            if (native_superframe_rc == 0) {
                                                have_native_superframe = 1;
                                                if (native_au_capture.sequence != superframe.generation) {
                                                    capture_superframe_aus(&superframe, &native_au_capture);
                                            }
                                        }
                                    }
                                }
                                    native_fresh = native_au_capture.valid != 0u &&
                                                   (native_valid_before == 0u ||
                                                    native_au_capture.sequence != native_sequence_before);
                                    if (native_fresh) {
                                        au_native_captures++;
                                        au_capture_queue_push(&native_au_queue, &native_au_capture);
                                    }
                                if (compare_au != 0) {
                                    enum {
                                        AU_COMPARE_FRAME_CAPACITY =
                                            DAB_MSC_CIFS_PER_FRAME * DAB_MSC_MAX_LOGICAL_FRAME_BYTES_PER_CIF
                                    };
                                    size_t ref_input_bytes = 0u;
                                    const dab_subch_runtime_t *ref_subch =
                                        &ref_ctx->tframe->msc_repo.subch[selected.subchid];
                                    uint8_t *ref_input_data = (uint8_t *)malloc(AU_COMPARE_FRAME_CAPACITY);

                                    if (ref_input_data == 0) {
                                        fprintf(stderr, "alloc failed for au compare payload\n");
                                        goto cleanup;
                                    }
                                    if (flatten_ours_payload(&ours_payload,
                                                             ref_input_data,
                                                             AU_COMPARE_FRAME_CAPACITY,
                                                             &ref_input_bytes) != 0) {
                                        free(ref_input_data);
                                        fprintf(stderr,
                                                "failed to flatten ours payload for au compare sid=0x%08x subch=%u\n",
                                                ours_payload.sid,
                                                selected.subchid);
                                        goto cleanup;
                                    }
                                    if (ref_input_bytes > 0u) {
                                        sync_dabplus(&ref_au_hooks,
                                                     (int)ref_subch->n,
                                                     ref_input_data,
                                                     (int)ref_input_bytes);
                                    }
                                    free(ref_input_data);
                                    ref_fresh = ref_au_capture.valid != 0u &&
                                                (ref_valid_before == 0u ||
                                                 ref_au_capture.sequence != ref_sequence_before);
                                    if (ref_fresh) {
                                        au_ref_captures++;
                                        au_capture_queue_push(&ref_au_queue, &ref_au_capture);
                                    }

                                    if (au_compare_started == 0) {
                                        if (au_capture_queue_find_alignment(&native_au_queue,
                                                                            &ref_au_queue,
                                                                            &native_align_index,
                                                                            &ref_align_index) != 0) {
                                            au_alignment_events++;
                                            au_alignment_drop_native += native_align_index;
                                            au_alignment_drop_ref += ref_align_index;
                                            au_capture_queue_drop_prefix(&native_au_queue, native_align_index);
                                            au_capture_queue_drop_prefix(&ref_au_queue, ref_align_index);
                                            au_compare_started = 1;
                                        } else {
                                            if (native_au_queue.count == AU_CAPTURE_QUEUE_CAPACITY ||
                                                ref_au_queue.count == AU_CAPTURE_QUEUE_CAPACITY) {
                                                if (native_au_queue.count > ref_au_queue.count) {
                                                    au_alignment_drop_native +=
                                                        (uint32_t)(native_au_queue.count - ref_au_queue.count);
                                                    au_capture_queue_drop_prefix(&native_au_queue,
                                                                                 (uint8_t)(native_au_queue.count - ref_au_queue.count));
                                                } else if (ref_au_queue.count > native_au_queue.count) {
                                                    au_alignment_drop_ref +=
                                                        (uint32_t)(ref_au_queue.count - native_au_queue.count);
                                                    au_capture_queue_drop_prefix(&ref_au_queue,
                                                                                 (uint8_t)(ref_au_queue.count - native_au_queue.count));
                                                } else {
                                                    au_alignment_drop_native++;
                                                    au_alignment_drop_ref++;
                                                    au_capture_queue_drop_prefix(&native_au_queue, 1u);
                                                    au_capture_queue_drop_prefix(&ref_au_queue, 1u);
                                                }
                                            }
                                        }
                                    }

                                    while (au_compare_started != 0 &&
                                           native_au_queue.count != 0u &&
                                           ref_au_queue.count != 0u) {
                                        int au_compare_rc = compare_au_capture(&native_au_queue.items[0],
                                                                               &ref_au_queue.items[0],
                                                                               compared_frames,
                                                                               tframe_result.frame_start_sample,
                                                                               ours_payload.sid,
                                                                               selected.subchid);
                                        if (au_compare_rc < 0) {
                                            goto cleanup;
                                        }
                                        if (au_compare_rc != 0) {
                                            au_mismatch_found = 1;
                                            goto cleanup;
                                        }
                                        au_matched_sets++;
                                        au_capture_queue_drop_prefix(&native_au_queue, 1u);
                                        au_capture_queue_drop_prefix(&ref_au_queue, 1u);
                                    }

                                    if (au_compare_started != 0 &&
                                        (native_au_queue.count > 1u || ref_au_queue.count > 1u)) {
                                        fprintf(stderr,
                                                "au-availability-mismatch frame=%u frame_start=%llu sid=0x%08x subch=%u native_pending=%u ref_pending=%u\n",
                                                compared_frames,
                                                (unsigned long long)tframe_result.frame_start_sample,
                                                ours_payload.sid,
                                                selected.subchid,
                                                (unsigned)native_au_queue.count,
                                                (unsigned)ref_au_queue.count);
                                        au_mismatch_found = 1;
                                        goto cleanup;
                                    }
                                }
                                }
                                if (play_audio != 0) {
                                    if (play_audio_superframe != 0) {
#if defined(DAB_HAVE_AUDIO_PLAYBACK)
                                        if (audio_component != 0) {
                                            if (have_native_superframe != 0 &&
                                                superframe.generation != audio.last_generation) {
                                                if (dab_audio_playback_consume_superframe(&audio, &superframe) != 0) {
                                                    goto cleanup;
                                                }
                                            } else {
                                                dab_msc_dabplus_debug_t debug = {0};
                                                int debug_rc;

                                                if (selected.sid != 0u) {
                                                    debug_rc =
                                                        dab_msc_get_primary_dabplus_debug(&msc, selected.sid, &debug);
                                                } else {
                                                    debug_rc = dab_msc_get_dabplus_debug(&msc, audio_component, &debug);
                                                }
                                                if (debug_rc == 0 &&
                                                    debug.valid != 0u) {
                                                    dab_audio_playback_report_dabplus_debug(&audio,
                                                                                            audio_component->sid,
                                                                                            &debug);
                                                } else {
                                                    dab_audio_playback_note_missing_superframe(&audio,
                                                                                              audio_component->sid,
                                                                                              msc.frames_seen);
                                                }
                                            }
                                        }
#endif
                                    } else if (play_audio_from_ours != 0) {
                                        enum {
                                            OURS_AUDIO_FRAME_CAPACITY =
                                                DAB_MSC_CIFS_PER_FRAME * DAB_MSC_MAX_LOGICAL_FRAME_BYTES_PER_CIF
                                        };
                                        size_t ours_audio_bytes = 0u;
                                        const dab_subch_runtime_t *ref_subch =
                                            &ref_ctx->tframe->msc_repo.subch[selected.subchid];
                                        uint8_t *ours_audio_data;

                                        ours_audio_data = (uint8_t *)malloc(OURS_AUDIO_FRAME_CAPACITY);
                                        if (ours_audio_data == 0) {
                                            fprintf(stderr, "alloc failed for ours audio payload\n");
                                            goto cleanup;
                                        }
                                        if (flatten_ours_payload(&ours_payload,
                                                                 ours_audio_data,
                                                                 OURS_AUDIO_FRAME_CAPACITY,
                                                                 &ours_audio_bytes) != 0) {
                                            fprintf(stderr,
                                                    "failed to flatten ours payload sid=0x%08x subch=%u\n",
                                                    ours_payload.sid,
                                                    selected.subchid);
                                            free(ours_audio_data);
                                            goto cleanup;
                                        }
                                        if (ours_audio_bytes > 0u) {
                                            sync_dabplus(ref_ctx->cbContext,
                                                         (int)ref_subch->n,
                                                         ours_audio_data,
                                                         (int)ours_audio_bytes);
                                        }
                                        free(ours_audio_data);
                                    } else {
                                        play_subch(ref_handle, selected.subchid);
                                    }
                                }
                                if (compare_rc == 1) {
                                    mismatch_found = 1;
                                    goto cleanup;
                                }
                                if (compare_rc == 2) {
                                    rc = 0;
                                    goto cleanup;
                                }
                                if (frame_limit > 0u && compared_frames >= frame_limit) {
                                    rc = 0;
                                    goto cleanup;
                                }
                            }
                        }
                    }
                }
            }
        }

        pending.pending = 0;
        pending.predicted = 0;
    }

    rc = (mismatch_found || au_mismatch_found) ? 1 : 0;
    if (!mismatch_found && !au_mismatch_found) {
        if (compare_au != 0) {
            printf("au-summary native_captures=%u ref_captures=%u matched_sets=%u alignment_events=%u dropped_native=%u dropped_ref=%u pending_native=%u pending_ref=%u started=%d\n",
                   au_native_captures,
                   au_ref_captures,
                   au_matched_sets,
                   au_alignment_events,
                   au_alignment_drop_native,
                   au_alignment_drop_ref,
                   (unsigned)native_au_queue.count,
                   (unsigned)ref_au_queue.count,
                   au_compare_started);
            if (final_native_debug_valid != 0) {
                printf("au-native-debug subch=%u sync=%u superframe_valid=%u columns=%u generation=%llu attempts=%u failures=%u rs_last=%u rs_total=%u last_ok=%u resets=%u reason=0x%02x\n",
                       (unsigned)final_native_debug.subchid,
                       final_native_debug.synchronized,
                       final_native_debug.superframe_valid,
                       final_native_debug.columns_filled,
                       (unsigned long long)final_native_debug.generation,
                       final_native_debug.decode_attempts,
                       final_native_debug.decode_failures,
                       final_native_debug.rs_corrected_last_superframe,
                       final_native_debug.rs_corrected_total,
                       final_native_debug.last_decode_ok,
                       final_native_debug.runtime_reset_count,
                       final_native_debug.last_reconfig_reason);
            }
        }
        printf("summary blocks=%llu compared_frames=%u selected_subch=%u locked=%d fic_frames=%u msc_frames=%u\n",
               (unsigned long long)blocks_read,
               compared_frames,
               (unsigned)selected.subchid,
               sync.state.locked,
               fic.frames_seen,
               msc.frames_seen);
    }

cleanup:
    if (play_audio != 0 && replay_clock_started != 0 && audio_drain_ms > 0u) {
        sleep_ms(audio_drain_ms);
    }
#if defined(DAB_HAVE_AUDIO_PLAYBACK)
    dab_audio_playback_close(&audio);
#endif
    if (ref_handle != 0) {
        sdrplay_api_shutdown(ref_handle);
    }
    if (fp != 0) {
        fclose(fp);
    }
    free(iq_block);
    free(sample_scratch);
    history_destroy(&history);
    dab_fic_destroy(&fic);
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
    free(active_q);
    free(active_i);
    free(fft_q);
    free(fft_i);
    return rc;
}
