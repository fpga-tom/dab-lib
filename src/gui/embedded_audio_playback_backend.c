#include "dab_gui_backend.h"

#include <stdio.h>
#include <signal.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "dab/core/msc.h"
#include "dab/os/os_mutex.h"
#include "dab/os/os_thread.h"
#include "audio_playback.h"
#include "receiver_session.h"

typedef struct {
    dab_thread_t thread;
    int thread_valid;
    int running;
    uint64_t frequency_hz;
    uint32_t sid;
} dab_gui_playback_state_t;

static dab_gui_playback_state_t g_playback_state = {0};
static dab_gui_playback_status_t g_playback_status = {0};
static dab_gui_playback_spectrum_t g_playback_spectrum = {0};
static dab_gui_playback_logo_t g_playback_logo = {0};
static dab_mutex_t g_playback_mutex;
static int g_playback_mutex_ready = 0;

typedef struct {
    uint8_t *data;
    size_t size;
    int present;
} dab_gui_mot_segment_t;

typedef struct {
    int current_transport_id;
    uint8_t *header;
    size_t header_size;
    uint32_t body_size;
    dab_gui_mot_segment_t *segments;
    size_t segment_capacity;
    int last_segment_received;
    uint16_t total_segments;
    uint8_t *accumulated_data;
    size_t accumulated_size;
    uint16_t expected_dg_length;
} dab_gui_mot_state_t;

enum {
    DAB_GUI_DLS_MAX_SEGMENT_BYTES = 48u,
    DAB_GUI_DLS_MAX_TEXT_BYTES = 256u,
    DAB_GUI_DLS_MAX_LABEL_SEGMENTS = 8u,
    DAB_GUI_DLS_MAX_LABEL_SEGMENT_BYTES = 16u,
    DAB_GUI_DLS_MAX_GROUP_BYTES = 64u
};

typedef struct {
    int active;
    uint8_t toggle;
    uint8_t group[DAB_GUI_DLS_MAX_GROUP_BYTES];
    size_t group_size;
    size_t group_expected_size;
    int label_active;
    uint8_t label_toggle;
    uint8_t label_charset;
    uint8_t label_segment_present[DAB_GUI_DLS_MAX_LABEL_SEGMENTS];
    uint8_t label_segment_lengths[DAB_GUI_DLS_MAX_LABEL_SEGMENTS];
    uint8_t label_segments[DAB_GUI_DLS_MAX_LABEL_SEGMENTS]
                          [DAB_GUI_DLS_MAX_LABEL_SEGMENT_BYTES];
    int label_last_segment;
} dab_gui_dls_state_t;

static dab_gui_mot_state_t g_mot_state = { .current_transport_id = -1 };
static dab_gui_dls_state_t g_dls_state = {0};
static int g_last_xpad_ci_type = -1;
static size_t g_last_xpad_ci_length = 0u;

static void dab_gui_playback_ensure_mutex(void) {
    if (g_playback_mutex_ready == 0) {
        (void)dab_mutex_init(&g_playback_mutex);
        g_playback_mutex_ready = 1;
    }
}

static void dab_gui_setenv_value(const char *name, const char *value) {
#if defined(_WIN32)
    (void)_putenv_s(name, value);
#else
    (void)setenv(name, value, 1);
#endif
}

static void dab_gui_setenv_real_backend(void) {
    dab_gui_setenv_value("DAB_SDRPLAY_USE_REAL", "1");
    dab_gui_setenv_value("DAB_RTLSDR_USE_REAL", "1");
}

static int dab_gui_env_enabled(const char *name) {
    const char *value = getenv(name);

    if (value == 0 || value[0] == '\0') {
        return 0;
    }
    return strcmp(value, "0") != 0 &&
           strcmp(value, "false") != 0 &&
           strcmp(value, "off") != 0;
}

static void dab_gui_dump_bytes_to_file(const char *path, const uint8_t *data, size_t size) {
    FILE *fp;

    if (path == 0 || data == 0 || size == 0u) {
        return;
    }

    fp = fopen(path, "wb");
    if (fp == 0) {
        return;
    }
    if (fwrite(data, 1u, size, fp) != size) {
        fclose(fp);
        return;
    }
    fclose(fp);
}

void dab_gui_embedded_audio_status_reset(void) {
    dab_gui_playback_ensure_mutex();
    dab_mutex_lock(&g_playback_mutex);
    g_playback_status.locked = 0;
    g_playback_status.has_receiver_snr = 0;
    g_playback_status.receiver_snr_db = 0.0f;
    g_playback_status.has_spectrum_cn = 0;
    g_playback_status.spectrum_cn_db = 0.0f;
    g_playback_status.has_rf_level = 0;
    g_playback_status.rf_level_dbfs = 0.0f;
    g_playback_status.prs_score = 0.0f;
    g_playback_status.now_playing[0] = '\0';
    g_playback_spectrum.valid = 0;
    g_playback_spectrum.sample_rate = 0u;
    g_playback_spectrum.bin_count = 0u;
    memset(g_playback_spectrum.bins, 0, sizeof(g_playback_spectrum.bins));
    g_playback_logo.valid = 0;
    g_playback_logo.byte_count = 0u;
    memset(g_playback_logo.bytes, 0, sizeof(g_playback_logo.bytes));
    dab_mutex_unlock(&g_playback_mutex);
}

static void dab_gui_embedded_audio_logo_publish(const uint8_t *data, size_t size) {
    if (data == 0 || size == 0u || size > DAB_GUI_PLAYBACK_LOGO_MAX_BYTES) {
        return;
    }

    if (dab_gui_env_enabled("DAB_GUI_DUMP_LOGO")) {
        dab_gui_dump_bytes_to_file("dab_gui_logo_dump.bin", data, size);
    }

    dab_gui_playback_ensure_mutex();
    dab_mutex_lock(&g_playback_mutex);
    g_playback_logo.valid = 1;
    g_playback_logo.generation++;
    g_playback_logo.byte_count = (uint32_t)size;
    memcpy(g_playback_logo.bytes, data, size);
    dab_mutex_unlock(&g_playback_mutex);
}

static void dab_gui_mot_reset_object(dab_gui_mot_state_t *state) {
    size_t i;

    if (state == 0) {
        return;
    }

    free(state->header);
    state->header = 0;
    state->header_size = 0u;
    state->body_size = 0u;

    for (i = 0u; i < state->segment_capacity; ++i) {
        free(state->segments[i].data);
        state->segments[i].data = 0;
        state->segments[i].size = 0u;
        state->segments[i].present = 0;
    }

    state->last_segment_received = 0;
    state->total_segments = 0u;
}

static void dab_gui_mot_reset_state(dab_gui_mot_state_t *state) {
    if (state == 0) {
        return;
    }

    dab_gui_mot_reset_object(state);
    free(state->segments);
    state->segments = 0;
    state->segment_capacity = 0u;

    free(state->accumulated_data);
    state->accumulated_data = 0;
    state->accumulated_size = 0u;
    state->expected_dg_length = 0u;
    state->current_transport_id = -1;
}

static int dab_gui_mot_ensure_segment_capacity(dab_gui_mot_state_t *state, uint16_t segment_number) {
    size_t new_capacity;
    dab_gui_mot_segment_t *segments;
    size_t i;

    if (state == 0) {
        return -1;
    }
    if ((size_t)segment_number < state->segment_capacity) {
        return 0;
    }

    new_capacity = state->segment_capacity == 0u ? 8u : state->segment_capacity;
    while (new_capacity <= (size_t)segment_number) {
        new_capacity *= 2u;
    }

    segments = (dab_gui_mot_segment_t *)realloc(state->segments, new_capacity * sizeof(*segments));
    if (segments == 0) {
        return -1;
    }
    for (i = state->segment_capacity; i < new_capacity; ++i) {
        segments[i] = (dab_gui_mot_segment_t){0};
    }

    state->segments = segments;
    state->segment_capacity = new_capacity;
    return 0;
}

static int dab_gui_mot_add_segment(dab_gui_mot_state_t *state,
                                   int is_header,
                                   uint16_t segment_number,
                                   int is_last_segment,
                                   const uint8_t *segment_data,
                                   size_t segment_size) {
    uint8_t *copy;

    if (state == 0 || segment_data == 0) {
        return -1;
    }

    copy = (uint8_t *)malloc(segment_size);
    if (copy == 0) {
        return -1;
    }
    memcpy(copy, segment_data, segment_size);

    if (is_header != 0) {
        free(state->header);
        state->header = copy;
        state->header_size = segment_size;
        state->body_size = 0u;
        if (segment_size >= 4u) {
            state->body_size = ((uint32_t)segment_data[0] << 20u) |
                               ((uint32_t)segment_data[1] << 12u) |
                               ((uint32_t)segment_data[2] << 4u) |
                               ((uint32_t)segment_data[3] >> 4u);
        }
    } else {
        if (dab_gui_mot_ensure_segment_capacity(state, segment_number) != 0) {
            free(copy);
            return -1;
        }
        free(state->segments[segment_number].data);
        state->segments[segment_number].data = copy;
        state->segments[segment_number].size = segment_size;
        state->segments[segment_number].present = 1;
    }

    if (is_last_segment != 0) {
        state->last_segment_received = 1;
        state->total_segments = (uint16_t)(segment_number + 1u);
    }

    return 0;
}

static int dab_gui_mot_is_complete(const dab_gui_mot_state_t *state) {
    uint16_t i;
    size_t body_bytes = 0u;

    if (state == 0 || state->last_segment_received == 0 || state->total_segments == 0u || state->header == 0) {
        return 0;
    }
    for (i = 0u; i < state->total_segments; ++i) {
        if ((size_t)i >= state->segment_capacity || state->segments[i].present == 0) {
            return 0;
        }
        body_bytes += state->segments[i].size;
    }
    if (state->body_size != 0u && body_bytes < (size_t)state->body_size) {
        return 0;
    }
    return 1;
}

static int dab_gui_mot_build_object(const dab_gui_mot_state_t *state, uint8_t **object_data, size_t *object_size) {
    uint8_t *data;
    size_t size = 0u;
    size_t offset = 0u;
    uint16_t i;

    if (state == 0 || object_data == 0 || object_size == 0 || dab_gui_mot_is_complete(state) == 0) {
        return -1;
    }

    size += state->header_size;
    for (i = 0u; i < state->total_segments; ++i) {
        size += state->segments[i].size;
    }

    data = (uint8_t *)malloc(size);
    if (data == 0) {
        return -1;
    }

    memcpy(data + offset, state->header, state->header_size);
    offset += state->header_size;
    for (i = 0u; i < state->total_segments; ++i) {
        memcpy(data + offset, state->segments[i].data, state->segments[i].size);
        offset += state->segments[i].size;
    }

    *object_data = data;
    *object_size = size;
    return 0;
}

static void dab_gui_mot_process_object_data(const uint8_t *object_data, size_t object_size) {
    uint16_t header_size;
    uint8_t content_type;
    size_t payload_size;
    const uint8_t *payload;
    size_t i;

    static const uint8_t kPngSignature[] = {0x89u, 'P', 'N', 'G', '\r', '\n', 0x1au, '\n'};
    static const uint8_t kJpegSignature[] = {0xffu, 0xd8u, 0xffu};
    static const uint8_t kGif87aSignature[] = {'G', 'I', 'F', '8', '7', 'a'};
    static const uint8_t kGif89aSignature[] = {'G', 'I', 'F', '8', '9', 'a'};
    static const uint8_t kBmpSignature[] = {'B', 'M'};
    static const uint8_t kPngIendChunk[] = {'I', 'E', 'N', 'D', 0xaeu, 0x42u, 0x60u, 0x82u};

    if (object_data == 0 || object_size < 7u) {
        return;
    }

    if (dab_gui_env_enabled("DAB_GUI_DUMP_MOT")) {
        dab_gui_dump_bytes_to_file("dab_gui_mot_object_dump.bin", object_data, object_size);
    }

    content_type = (uint8_t)((object_data[5] & 0x7Fu) >> 1u);
    header_size = (uint16_t)(((uint16_t)(object_data[3] & 0x0Fu) << 9u) |
                             ((uint16_t)object_data[4] << 1u) |
                             ((uint16_t)object_data[5] >> 7u));

    if (header_size >= object_size) {
        return;
    }
    if (content_type == 0x02u) {
        payload = object_data + header_size;
        payload_size = object_size - header_size;

        if ((payload_size >= sizeof(kPngSignature) &&
             memcmp(payload, kPngSignature, sizeof(kPngSignature)) == 0) ||
            (payload_size >= sizeof(kJpegSignature) &&
             memcmp(payload, kJpegSignature, sizeof(kJpegSignature)) == 0) ||
            (payload_size >= sizeof(kGif87aSignature) &&
             memcmp(payload, kGif87aSignature, sizeof(kGif87aSignature)) == 0) ||
            (payload_size >= sizeof(kGif89aSignature) &&
             memcmp(payload, kGif89aSignature, sizeof(kGif89aSignature)) == 0) ||
            (payload_size >= sizeof(kBmpSignature) &&
             memcmp(payload, kBmpSignature, sizeof(kBmpSignature)) == 0)) {
            if (payload_size >= sizeof(kPngSignature) &&
                memcmp(payload, kPngSignature, sizeof(kPngSignature)) == 0) {
                if (payload_size < sizeof(kPngIendChunk) ||
                    memcmp(payload + payload_size - sizeof(kPngIendChunk),
                           kPngIendChunk,
                           sizeof(kPngIendChunk)) != 0) {
                    return;
                }
            }
            dab_gui_embedded_audio_logo_publish(payload, payload_size);
            return;
        }

        for (i = 0u; i < payload_size; ++i) {
            const size_t remaining = payload_size - i;
            const uint8_t *candidate = payload + i;

            if ((remaining >= sizeof(kPngSignature) &&
                 memcmp(candidate, kPngSignature, sizeof(kPngSignature)) == 0) ||
                (remaining >= sizeof(kJpegSignature) &&
                 memcmp(candidate, kJpegSignature, sizeof(kJpegSignature)) == 0) ||
                (remaining >= sizeof(kGif87aSignature) &&
                 memcmp(candidate, kGif87aSignature, sizeof(kGif87aSignature)) == 0) ||
                (remaining >= sizeof(kGif89aSignature) &&
                 memcmp(candidate, kGif89aSignature, sizeof(kGif89aSignature)) == 0) ||
                (remaining >= sizeof(kBmpSignature) &&
                 memcmp(candidate, kBmpSignature, sizeof(kBmpSignature)) == 0)) {
                if (remaining >= sizeof(kPngSignature) &&
                    memcmp(candidate, kPngSignature, sizeof(kPngSignature)) == 0) {
                    if (remaining < sizeof(kPngIendChunk) ||
                        memcmp(candidate + remaining - sizeof(kPngIendChunk),
                               kPngIendChunk,
                               sizeof(kPngIendChunk)) != 0) {
                        return;
                    }
                }
                dab_gui_embedded_audio_logo_publish(candidate, remaining);
                break;
            }
        }
    }
}

static int dab_gui_mot_parse_data_group_header(const uint8_t *dg,
                                               size_t dg_size,
                                               size_t *offset,
                                               uint8_t *dg_type) {
    uint8_t flags;

    if (dg == 0 || offset == 0 || dg_type == 0 || dg_size < *offset + 2u) {
        return -1;
    }

    flags = dg[*offset];
    *dg_type = (uint8_t)(flags & 0x0Fu);
    *offset += 2u + ((flags & 0x80u) != 0u ? 2u : 0u);

    if ((flags & 0x40u) == 0u || (flags & 0x20u) == 0u || (flags & 0x10u) == 0u) {
        return -1;
    }
    if (*dg_type != 3u && *dg_type != 4u) {
        return -1;
    }
    return 0;
}

static int dab_gui_mot_parse_session_header(const uint8_t *dg,
                                            size_t dg_size,
                                            size_t *offset,
                                            int *last_segment,
                                            uint16_t *segment_number,
                                            uint16_t *transport_id) {
    uint8_t len_indicator;

    if (dg == 0 || offset == 0 || last_segment == 0 || segment_number == 0 || transport_id == 0 ||
        dg_size < *offset + 3u) {
        return -1;
    }

    *last_segment = (dg[*offset] & 0x80u) != 0u;
    *segment_number = (uint16_t)(((uint16_t)(dg[*offset] & 0x7Fu) << 8u) | (uint16_t)dg[*offset + 1u]);
    if ((dg[*offset + 2u] & 0x10u) == 0u) {
        return -1;
    }
    len_indicator = (uint8_t)(dg[*offset + 2u] & 0x0Fu);
    *offset += 3u;
    if (len_indicator < 2u || dg_size < *offset + len_indicator) {
        return -1;
    }

    *transport_id = (uint16_t)(((uint16_t)dg[*offset] << 8u) | (uint16_t)dg[*offset + 1u]);
    *offset += len_indicator;
    return 0;
}

static int dab_gui_mot_parse_segmentation_header(const uint8_t *dg,
                                                 size_t dg_size,
                                                 size_t *offset,
                                                 size_t *segment_size) {
    if (dg == 0 || offset == 0 || segment_size == 0 || dg_size < *offset + 2u) {
        return -1;
    }

    *segment_size = (size_t)(((uint16_t)(dg[*offset] & 0x1Fu) << 8u) | (uint16_t)dg[*offset + 1u]);
    *offset += 2u;

    if (*segment_size != dg_size - *offset - 2u) {
        return -1;
    }
    return 0;
}

static void dab_gui_mot_consume_data_group(dab_gui_mot_state_t *state, const uint8_t *dg, size_t dg_size) {
    size_t offset = 0u;
    uint8_t dg_type = 0u;
    int last_segment = 0;
    uint16_t segment_number = 0u;
    uint16_t transport_id = 0u;
    size_t segment_size = 0u;
    uint8_t *object_data = 0;
    size_t object_size = 0u;

    if (state == 0 || dg == 0 || dg_size == 0u) {
        return;
    }
    if (dab_gui_mot_parse_data_group_header(dg, dg_size, &offset, &dg_type) != 0 ||
        dab_gui_mot_parse_session_header(dg, dg_size, &offset, &last_segment, &segment_number, &transport_id) != 0 ||
        dab_gui_mot_parse_segmentation_header(dg, dg_size, &offset, &segment_size) != 0 ||
        offset + segment_size > dg_size) {
        return;
    }

    if (state->current_transport_id != (int)transport_id) {
        dab_gui_mot_reset_object(state);
        state->current_transport_id = (int)transport_id;
    }

    if (dab_gui_mot_add_segment(state,
                                dg_type == 3u,
                                segment_number,
                                last_segment,
                                dg + offset,
                                segment_size) != 0) {
        return;
    }

    if (segment_number > 0u &&
        dab_gui_mot_build_object(state, &object_data, &object_size) == 0) {
        dab_gui_mot_process_object_data(object_data, object_size);
        free(object_data);
        dab_gui_mot_reset_object(state);
    }
}

static int dab_gui_xpad_ci_payload_size(uint8_t ci) {
    switch ((ci >> 5u) & 0x07u) {
        case 0u: return 4;
        case 1u: return 6;
        case 2u: return 8;
        case 3u: return 12;
        case 4u: return 16;
        case 5u: return 24;
        case 6u: return 32;
        case 7u: return 48;
        default: return -1;
    }
}

static void dab_gui_dls_process_subfield(int start,
                                         const uint8_t *data,
                                         size_t size);
static int dab_gui_mot_append_accumulated(dab_gui_mot_state_t *state,
                                          const uint8_t *data,
                                          size_t size,
                                          int reset);

static void dab_gui_process_xpad_application(uint8_t application_type,
                                             const uint8_t *payload,
                                             size_t payload_size) {
    if (application_type == 2u || application_type == 3u) {
        dab_gui_dls_process_subfield(application_type == 2u, payload, payload_size);
    } else if (application_type == 12u) {
        if (dab_gui_mot_append_accumulated(&g_mot_state,
                                           payload,
                                           payload_size,
                                           1) != 0) {
            return;
        }
    } else if (application_type == 13u) {
        if (dab_gui_mot_append_accumulated(&g_mot_state,
                                           payload,
                                           payload_size,
                                           0) != 0) {
            return;
        }
    } else if (application_type == 1u && payload_size >= 2u) {
        g_mot_state.expected_dg_length =
            (uint16_t)((((uint16_t)payload[0] & 0x3fu) << 8u) |
                       (uint16_t)payload[1]);
    }

    if (g_mot_state.expected_dg_length != 0u &&
        g_mot_state.accumulated_size >=
            (size_t)g_mot_state.expected_dg_length) {
        dab_gui_mot_consume_data_group(&g_mot_state,
                                       g_mot_state.accumulated_data,
                                       g_mot_state.expected_dg_length);
        free(g_mot_state.accumulated_data);
        g_mot_state.accumulated_data = 0;
        g_mot_state.accumulated_size = 0u;
        g_mot_state.expected_dg_length = 0u;
    }
}

static void dab_gui_dls_reset(void) {
    memset(&g_dls_state, 0, sizeof(g_dls_state));
    g_dls_state.label_last_segment = -1;
    g_last_xpad_ci_type = -1;
    g_last_xpad_ci_length = 0u;
}

static uint16_t dab_gui_dls_crc16(const uint8_t *data, size_t size) {
    uint16_t crc = 0xffffu;
    size_t byte_index;

    for (byte_index = 0u; byte_index < size; ++byte_index) {
        uint8_t bit;
        crc ^= (uint16_t)data[byte_index] << 8u;
        for (bit = 0u; bit < 8u; ++bit) {
            crc = (crc & 0x8000u) != 0u
                      ? (uint16_t)((crc << 1u) ^ 0x1021u)
                      : (uint16_t)(crc << 1u);
        }
    }
    return (uint16_t)~crc;
}

static void dab_gui_dls_publish(const uint8_t *text, size_t text_size) {
    char utf8[DAB_GUI_DLS_MAX_TEXT_BYTES];
    size_t source_offset = 0u;
    size_t output_size = 0u;

    if (text == 0) {
        return;
    }

    while (source_offset < text_size && output_size + 2u < sizeof(utf8)) {
        const uint8_t character = text[source_offset++];
        if (character == '\r') {
            break;
        }
        if (character < 0x20u || (character >= 0x7fu && character <= 0x9fu)) {
            continue;
        }
        if (character < 0x80u) {
            utf8[output_size++] = (char)character;
        } else {
            utf8[output_size++] = (char)(0xc0u | (character >> 6u));
            utf8[output_size++] = (char)(0x80u | (character & 0x3fu));
        }
    }

    while (output_size > 0u && utf8[output_size - 1u] == ' ') {
        --output_size;
    }
    utf8[output_size] = '\0';

    dab_gui_playback_ensure_mutex();
    dab_mutex_lock(&g_playback_mutex);
    memcpy(g_playback_status.now_playing, utf8, output_size + 1u);
    dab_mutex_unlock(&g_playback_mutex);
}

static void dab_gui_dls_process_group(const uint8_t *group, size_t group_size) {
    uint16_t received_crc;
    uint16_t calculated_crc;
    uint8_t header;
    uint8_t toggle;
    uint8_t first_segment;
    uint8_t last_segment;
    uint8_t segment_number;
    size_t text_size;
    uint8_t assembled[DAB_GUI_DLS_MAX_TEXT_BYTES];
    size_t assembled_size = 0u;
    size_t index;

    if (group == 0 || group_size < 4u) {
        return;
    }

    received_crc = (uint16_t)(((uint16_t)group[group_size - 2u] << 8u) |
                              group[group_size - 1u]);
    calculated_crc = dab_gui_dls_crc16(group, group_size - 2u);
    if (received_crc != calculated_crc) {
        return;
    }

    header = group[0];
    if ((header & 0x10u) != 0u) {
        if ((header & 0x0fu) == 1u) {
            dab_gui_playback_ensure_mutex();
            dab_mutex_lock(&g_playback_mutex);
            g_playback_status.now_playing[0] = '\0';
            dab_mutex_unlock(&g_playback_mutex);
        }
        return;
    }

    text_size = (size_t)(header & 0x0fu) + 1u;
    if (group_size != text_size + 4u) {
        return;
    }

    toggle = (uint8_t)(header >> 7u);
    first_segment = (uint8_t)((header & 0x40u) != 0u);
    last_segment = (uint8_t)((header & 0x20u) != 0u);
    segment_number = first_segment != 0u
                         ? 0u
                         : (uint8_t)((group[1] & 0x70u) >> 4u);
    if (segment_number >= DAB_GUI_DLS_MAX_LABEL_SEGMENTS) {
        return;
    }

    if (first_segment != 0u || g_dls_state.label_active == 0 ||
        g_dls_state.label_toggle != toggle) {
        if (first_segment == 0u) {
            return;
        }
        memset(g_dls_state.label_segment_present, 0,
               sizeof(g_dls_state.label_segment_present));
        g_dls_state.label_last_segment = -1;
        g_dls_state.label_active = 1;
        g_dls_state.label_toggle = toggle;
        g_dls_state.label_charset = (uint8_t)(group[1] >> 4u);
    }

    if (text_size > DAB_GUI_DLS_MAX_LABEL_SEGMENT_BYTES) {
        return;
    }
    memcpy(g_dls_state.label_segments[segment_number], group + 2u, text_size);
    g_dls_state.label_segment_lengths[segment_number] = (uint8_t)text_size;
    g_dls_state.label_segment_present[segment_number] = 1u;
    if (last_segment != 0u) {
        g_dls_state.label_last_segment = (int)segment_number;
    }

    if (g_dls_state.label_last_segment < 0) {
        return;
    }
    for (index = 0u; index <= (size_t)g_dls_state.label_last_segment; ++index) {
        size_t segment_size;
        if (g_dls_state.label_segment_present[index] == 0u) {
            return;
        }
        segment_size = g_dls_state.label_segment_lengths[index];
        if (segment_size > sizeof(assembled) - assembled_size) {
            return;
        }
        memcpy(assembled + assembled_size,
               g_dls_state.label_segments[index],
               segment_size);
        assembled_size += segment_size;
    }
    dab_gui_dls_publish(assembled, assembled_size);
}

static void dab_gui_dls_process_subfield(int start, const uint8_t *data, size_t size) {
    size_t copied_size = 0u;

    if (data == 0 || size == 0u) {
        return;
    }
    if (start != 0) {
        g_dls_state.group_size = 0u;
        g_dls_state.group_expected_size = 0u;
        g_dls_state.active = 1;
    } else if (g_dls_state.active == 0) {
        return;
    }

    while (copied_size < size &&
           g_dls_state.group_size < sizeof(g_dls_state.group)) {
        size_t remaining = sizeof(g_dls_state.group) - g_dls_state.group_size;
        if (g_dls_state.group_expected_size != 0u) {
            if (g_dls_state.group_size >= g_dls_state.group_expected_size) {
                dab_gui_dls_process_group(g_dls_state.group,
                                          g_dls_state.group_expected_size);
                g_dls_state.group_size = 0u;
                g_dls_state.group_expected_size = 0u;
                g_dls_state.active = 0;
                break;
            }
            remaining = g_dls_state.group_expected_size - g_dls_state.group_size;
        }
        if (remaining > size - copied_size) {
            remaining = size - copied_size;
        }
        memcpy(g_dls_state.group + g_dls_state.group_size,
               data + copied_size,
               remaining);
        g_dls_state.group_size += remaining;
        copied_size += remaining;

        if (g_dls_state.group_expected_size == 0u &&
            g_dls_state.group_size >= 2u) {
            const uint8_t header = g_dls_state.group[0];
            size_t field_size;
            if ((header & 0x10u) == 0u) {
                field_size = (size_t)(header & 0x0fu) + 1u;
            } else if ((header & 0x0fu) == 1u) {
                field_size = 0u;
            } else if ((header & 0x0fu) == 2u) {
                field_size = (size_t)(g_dls_state.group[1] & 0x0fu) + 1u;
            } else {
                g_dls_state.group_size = 0u;
                g_dls_state.group_expected_size = 0u;
                g_dls_state.active = 0;
                return;
            }
            g_dls_state.group_expected_size = 2u + field_size + 2u;
        }
        if (g_dls_state.group_expected_size != 0u &&
            g_dls_state.group_size >= g_dls_state.group_expected_size) {
            dab_gui_dls_process_group(g_dls_state.group,
                                      g_dls_state.group_expected_size);
            g_dls_state.group_size = 0u;
            g_dls_state.group_expected_size = 0u;
            g_dls_state.active = 0;
            break;
        }
    }
}

static int dab_gui_mot_append_accumulated(dab_gui_mot_state_t *state, const uint8_t *data, size_t size, int reset) {
    uint8_t *combined;

    if (state == 0 || (data == 0 && size != 0u)) {
        return -1;
    }
    if (reset != 0) {
        free(state->accumulated_data);
        state->accumulated_data = 0;
        state->accumulated_size = 0u;
    }
    if (size == 0u) {
        return 0;
    }

    combined = (uint8_t *)realloc(state->accumulated_data, state->accumulated_size + size);
    if (combined == 0) {
        return -1;
    }
    memcpy(combined + state->accumulated_size, data, size);
    state->accumulated_data = combined;
    state->accumulated_size += size;
    return 0;
}

static void dab_gui_process_pad(const uint8_t *xpad_data, size_t xpad_len, const uint8_t *fpad_data) {
    uint8_t reversed[196];
    uint8_t ci_values[4] = {0};
    size_t ci_lengths[4] = {0u};
    uint8_t ci_count = 0u;
    size_t offset = 0u;
    size_t i;
    uint8_t fpad_type;
    uint8_t xpad_indicator;
    int ci_flag;
    int next_ci_type = -1;
    size_t next_ci_length = 0u;

    if (xpad_data == 0 || fpad_data == 0 || xpad_len == 0u || xpad_len > sizeof(reversed)) {
        return;
    }

    fpad_type = (uint8_t)(fpad_data[0] >> 6u);
    xpad_indicator = (uint8_t)((fpad_data[0] & 0x30u) >> 4u);
    ci_flag = (fpad_data[1] & 0x02u) != 0u;

    if (fpad_type != 0u) {
        return;
    }

    for (i = 0u; i < xpad_len; ++i) {
        reversed[i] = xpad_data[xpad_len - 1u - i];
    }

    if (ci_flag != 0) {
        if (xpad_indicator == 0x02u) {
            while (ci_count < 4u && offset < xpad_len) {
                const uint8_t ci = reversed[offset++];
                if ((ci & 0x1fu) == 0u) {
                    break;
                }
                ci_values[ci_count] = ci;
                ci_lengths[ci_count] = (size_t)dab_gui_xpad_ci_payload_size(ci);
                ++ci_count;
            }
        } else if (xpad_indicator == 0x01u && xpad_len >= 4u) {
            const uint8_t ci = reversed[offset++];
            if ((ci & 0x1fu) != 0u) {
                ci_values[0] = ci;
                ci_lengths[0] = 3u;
                ci_count = 1u;
            }
        } else {
            return;
        }
    } else {
        if ((xpad_indicator != 0x01u && xpad_indicator != 0x02u) ||
            g_last_xpad_ci_type < 0 ||
            g_last_xpad_ci_length == 0u) {
            return;
        }
        ci_values[0] = (uint8_t)g_last_xpad_ci_type;
        ci_lengths[0] = g_last_xpad_ci_length;
        ci_count = 1u;
    }

    for (i = 0u; i < ci_count; ++i) {
        const uint8_t application_type = (uint8_t)(ci_values[i] & 0x1fu);
        const size_t payload_size = ci_flag != 0
                                        ? ci_lengths[i]
                                        : g_last_xpad_ci_length;
        const uint8_t *payload;

        if (payload_size == 0u || payload_size > xpad_len - offset) {
            break;
        }

        payload = reversed + offset;
        dab_gui_process_xpad_application(application_type, payload, payload_size);
        offset += payload_size;

        if (application_type == 1u) {
            next_ci_type = 1;
            next_ci_length = payload_size;
        } else if (application_type == 2u || application_type == 3u) {
            next_ci_type = 3;
            next_ci_length = payload_size;
        } else if (application_type == 12u || application_type == 13u) {
            next_ci_type = 13;
            next_ci_length = payload_size;
        }
    }
    g_last_xpad_ci_type = next_ci_type;
    g_last_xpad_ci_length = next_ci_length;
}

static void dab_gui_process_au_pad(const uint8_t *au_data, size_t au_len) {
    size_t pad_start = 2u;
    size_t pad_len;

    if (au_data == 0 || au_len < 3u || (au_data[0] >> 5u) != 4u) {
        return;
    }

    pad_len = au_data[1];
    if (pad_len == 255u) {
        if (au_len < 4u) {
            return;
        }
        pad_len += au_data[2];
        pad_start++;
    }

    if (pad_len < 2u || au_len < pad_start + pad_len) {
        return;
    }

    dab_gui_process_pad(au_data + pad_start, pad_len - 2u, au_data + pad_start + pad_len - 2u);
}

void dab_gui_embedded_audio_process_superframe(const dab_msc_dabplus_superframe_t *superframe) {
    uint8_t i;

    if (superframe == 0 || superframe->valid == 0u || superframe->superframe_data == 0) {
        return;
    }

    for (i = 0u; i < superframe->au_count; ++i) {
        const uint32_t start = superframe->au_start[i];
        const uint32_t len = superframe->au_length[i];

        if (start + len > superframe->superframe_bytes) {
            continue;
        }
        dab_gui_process_au_pad(superframe->superframe_data + start, len);
    }
}

void dab_gui_embedded_audio_status_update(int locked,
                                          int has_receiver_snr,
                                          float receiver_snr_db,
                                          int has_spectrum_cn,
                                          float spectrum_cn_db,
                                          int has_rf_level,
                                          float rf_level_dbfs,
                                          float prs_score) {
    dab_gui_playback_ensure_mutex();
    dab_mutex_lock(&g_playback_mutex);
    g_playback_status.locked = locked;
    g_playback_status.has_receiver_snr = has_receiver_snr;
    g_playback_status.receiver_snr_db = receiver_snr_db;
    g_playback_status.has_spectrum_cn = has_spectrum_cn;
    g_playback_status.spectrum_cn_db = spectrum_cn_db;
    g_playback_status.has_rf_level = has_rf_level;
    g_playback_status.rf_level_dbfs = rf_level_dbfs;
    g_playback_status.prs_score = prs_score;
    dab_mutex_unlock(&g_playback_mutex);
}

static void dab_gui_embedded_audio_spectrum_callback(const float *bins,
                                                     uint32_t bin_count,
                                                     uint32_t sample_rate,
                                                     void *user) {
    uint32_t copy_count = bin_count;
    (void)user;

    if (bins == 0) {
        return;
    }
    if (copy_count > DAB_AUDIO_SPECTRUM_DISPLAY_BINS) {
        copy_count = DAB_AUDIO_SPECTRUM_DISPLAY_BINS;
    }

    dab_gui_playback_ensure_mutex();
    dab_mutex_lock(&g_playback_mutex);
    memset(g_playback_spectrum.bins, 0, sizeof(g_playback_spectrum.bins));
    memcpy(g_playback_spectrum.bins, bins, (size_t)copy_count * sizeof(float));
    g_playback_spectrum.valid = 1;
    g_playback_spectrum.sample_rate = sample_rate;
    g_playback_spectrum.bin_count = copy_count;
    dab_mutex_unlock(&g_playback_mutex);
}

static int dab_gui_playback_thread_main(void *arg) {
    dab_gui_playback_state_t *state = (dab_gui_playback_state_t *)arg;
    dab_gui_sdr_settings_t settings;
    char freq_arg[32];
    char sid_arg[32];
    char gain_arg[16];
    char *argv[10];

    dab_gui_sdr_get_settings(&settings);
    snprintf(freq_arg, sizeof(freq_arg), "%llu", (unsigned long long)state->frequency_hz);
    snprintf(sid_arg, sizeof(sid_arg), "0x%08x", state->sid);
    snprintf(gain_arg, sizeof(gain_arg), "%d", settings.gain_db);

    argv[0] = "dab_gui_embedded_audio";
    argv[1] = "--freq";
    argv[2] = freq_arg;
    argv[3] = "--agc";
    argv[4] = settings.agc_enabled != 0 ? "on" : "off";
    argv[5] = "--debug";
    argv[6] = "off";
    argv[7] = "--sid";
    argv[8] = sid_arg;
    argv[9] = 0;

    dab_gui_setenv_real_backend();
    dab_gui_setenv_value("DAB_SDR_BACKEND", settings.backend == DAB_GUI_SDR_BACKEND_RTLSDR ? "rtlsdr" : "sdrplay");
    dab_gui_setenv_value("DAB_SYNC_PROBE_GAIN_DB", gain_arg);
    dab_receiver_session_reset_stop();
    dab_gui_embedded_audio_status_reset();
    dab_audio_playback_set_spectrum_callback(&dab_gui_embedded_audio_spectrum_callback, 0);
    (void)dab_receiver_session_run(9, argv);
    dab_audio_playback_set_spectrum_callback(0, 0);
    dab_gui_mot_reset_state(&g_mot_state);
    dab_gui_dls_reset();

    dab_gui_playback_ensure_mutex();
    dab_mutex_lock(&g_playback_mutex);
    g_playback_state.running = 0;
    g_playback_status.running = 0;
    g_playback_status.frequency_hz = g_playback_state.frequency_hz;
    g_playback_status.sid = g_playback_state.sid;
    g_playback_spectrum.valid = 0;
    g_playback_spectrum.sample_rate = 0u;
    g_playback_spectrum.bin_count = 0u;
    memset(g_playback_spectrum.bins, 0, sizeof(g_playback_spectrum.bins));
    g_playback_logo.valid = 0;
    g_playback_logo.byte_count = 0u;
    memset(g_playback_logo.bytes, 0, sizeof(g_playback_logo.bytes));
    g_playback_status.now_playing[0] = '\0';
    dab_mutex_unlock(&g_playback_mutex);
    return 0;
}

int dab_gui_playback_start(uint64_t frequency_hz, uint32_t sid) {
    int rc = -1;

    dab_gui_playback_ensure_mutex();
    dab_mutex_lock(&g_playback_mutex);
    if (!g_playback_state.running) {
        if (g_playback_state.thread_valid) {
            (void)dab_thread_join(&g_playback_state.thread, 0);
            g_playback_state.thread_valid = 0;
        }
        g_playback_state.frequency_hz = frequency_hz;
        g_playback_state.sid = sid;
        g_playback_state.running = 1;
        dab_gui_mot_reset_state(&g_mot_state);
        dab_gui_dls_reset();
        g_playback_status = (dab_gui_playback_status_t){
            .running = 1,
            .locked = 0,
            .has_receiver_snr = 0,
            .receiver_snr_db = 0.0f,
            .has_spectrum_cn = 0,
            .spectrum_cn_db = 0.0f,
            .has_rf_level = 0,
            .rf_level_dbfs = 0.0f,
            .prs_score = 0.0f,
            .frequency_hz = frequency_hz,
            .sid = sid,
        };
        rc = dab_thread_create(&g_playback_state.thread,
                               dab_gui_playback_thread_main,
                               &g_playback_state,
                               "dab_gui_playback");
        if (rc != 0) {
            g_playback_state.running = 0;
        } else {
            g_playback_state.thread_valid = 1;
        }
    }
    dab_mutex_unlock(&g_playback_mutex);

    return rc;
}

void dab_gui_playback_stop(void) {
    int should_join = 0;

    dab_gui_playback_ensure_mutex();
    dab_mutex_lock(&g_playback_mutex);
    if (g_playback_state.running) {
        dab_receiver_session_request_stop();
        should_join = 1;
    }
    dab_mutex_unlock(&g_playback_mutex);

    if (should_join) {
        (void)dab_thread_join(&g_playback_state.thread, 0);
        dab_mutex_lock(&g_playback_mutex);
        g_playback_state.thread_valid = 0;
        g_playback_state.running = 0;
        g_playback_status.running = 0;
        g_playback_status.locked = 0;
        g_playback_status.has_receiver_snr = 0;
        g_playback_status.has_spectrum_cn = 0;
        g_playback_status.has_rf_level = 0;
        g_playback_spectrum.valid = 0;
        g_playback_spectrum.sample_rate = 0u;
        g_playback_spectrum.bin_count = 0u;
        memset(g_playback_spectrum.bins, 0, sizeof(g_playback_spectrum.bins));
        g_playback_logo.valid = 0;
        g_playback_logo.byte_count = 0u;
        memset(g_playback_logo.bytes, 0, sizeof(g_playback_logo.bytes));
        g_playback_status.now_playing[0] = '\0';
        dab_mutex_unlock(&g_playback_mutex);
    }
}

int dab_gui_playback_is_running(void) {
    int running;

    dab_gui_playback_ensure_mutex();
    dab_mutex_lock(&g_playback_mutex);
    running = g_playback_state.running;
    dab_mutex_unlock(&g_playback_mutex);
    return running;
}

int dab_gui_playback_get_status(dab_gui_playback_status_t *status) {
    if (status == 0) {
        return -1;
    }

    dab_gui_playback_ensure_mutex();
    dab_mutex_lock(&g_playback_mutex);
    *status = g_playback_status;
    dab_mutex_unlock(&g_playback_mutex);
    return 0;
}

int dab_gui_playback_get_spectrum(dab_gui_playback_spectrum_t *spectrum) {
    if (spectrum == 0) {
        return -1;
    }

    dab_gui_playback_ensure_mutex();
    dab_mutex_lock(&g_playback_mutex);
    *spectrum = g_playback_spectrum;
    dab_mutex_unlock(&g_playback_mutex);
    return 0;
}

int dab_gui_playback_get_logo(dab_gui_playback_logo_t *logo) {
    if (logo == 0) {
        return -1;
    }

    dab_gui_playback_ensure_mutex();
    dab_mutex_lock(&g_playback_mutex);
    *logo = g_playback_logo;
    dab_mutex_unlock(&g_playback_mutex);
    return 0;
}

int dab_gui_playback_set_equalizer(const float *gains_db, uint32_t count) {
    dab_audio_playback_set_equalizer_gains(gains_db, count);
    return 0;
}
