#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "dab/config.h"
#include "dab/core/fic.h"
#include "dab/core/null_detect.h"
#include "dab/core/ofdm.h"
#include "dab/core/prs_acquire.h"
#include "dab/core/prs_fft.h"
#include "dab/core/prs_score.h"
#include "dab/core/sync.h"
#include "dab/core/tframe.h"
#include "dab/hal/sdr_device.h"
#include "dab/hal/sdrplay_device.h"

typedef struct {
    const char *channel;
    uint64_t frequency_hz;
} dab_channel_t;

typedef struct {
    uint32_t sid;
    char label[64];
    int used;
} scanned_service_t;

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
    dab_null_detect_t detector;
    dab_prs_acquire_t acquire;
    dab_prs_score_t cp_score;
    dab_prs_fft_t fft;
    dab_ofdm_t ofdm;
    dab_sync_t sync;
    dab_tframe_t tframe;
    dab_fic_state_t fic;
    history_buffer_t history;
    pending_candidate_t pending;
    dab_tframe_buffers_t tframe_buffers;
    float *fft_i;
    float *fft_q;
    float *active_i;
    float *active_q;
    float *prev_active_i;
    float *prev_active_q;
    float *diff_i;
    float *diff_q;
    float *deintl_i;
    float *deintl_q;
    int8_t *soft_a;
    int8_t *soft_b;
    int8_t *fic_softbits;
    int8_t *msc_softbits;
    int8_t *frame_softbits;
    uint64_t last_predicted_fft_start;
    float ref_threshold;
    float prs_fine_timing_limit;
    uint32_t symbol_samples;
    uint32_t fft_samples;
} scan_runtime_t;

static const dab_channel_t kBandIIIChannels[] = {
    {"5A", 174928000ull}, {"5B", 176640000ull}, {"5C", 178352000ull}, {"5D", 180064000ull},
    {"6A", 181936000ull}, {"6B", 183648000ull}, {"6C", 185360000ull}, {"6D", 187072000ull},
    {"7A", 188928000ull}, {"7B", 190640000ull}, {"7C", 192352000ull}, {"7D", 194064000ull},
    {"8A", 195936000ull}, {"8B", 197648000ull}, {"8C", 199360000ull}, {"8D", 201072000ull},
    {"9A", 202928000ull}, {"9B", 204640000ull}, {"9C", 206352000ull}, {"9D", 208064000ull},
    {"10A", 209936000ull}, {"10B", 211648000ull}, {"10C", 213360000ull}, {"10D", 215072000ull},
    {"11A", 216928000ull}, {"11B", 218640000ull}, {"11C", 220352000ull}, {"11D", 222064000ull},
    {"12A", 223936000ull}, {"12B", 225648000ull}, {"12C", 227360000ull}, {"12D", 229072000ull},
    {"13A", 230784000ull}, {"13B", 232496000ull}, {"13C", 234208000ull}, {"13D", 235776000ull},
    {"13E", 237488000ull}, {"13F", 239200000ull},
};

enum {
    DAB_CHANNEL_COUNT = (int)(sizeof(kBandIIIChannels) / sizeof(kBandIIIChannels[0]))
};

static void print_usage(const char *argv0) {
    fprintf(stderr,
            "usage: %s --output <path> [--agc on|off] [--per-frequency-seconds <seconds>]\n",
            argv0 != 0 ? argv0 : "dab_sdrplay_scan");
}

static int parse_u32_arg(const char *value, uint32_t *out) {
    char *end = 0;
    unsigned long parsed;

    if (value == 0 || out == 0) {
        return -1;
    }
    parsed = strtoul(value, &end, 10);
    if (end == value || (end != 0 && *end != '\0')) {
        return -1;
    }
    *out = (uint32_t)parsed;
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

static int build_sibling_tool_path(const char *self_path,
                                   const char *tool_name,
                                   char *out,
                                   size_t out_size) {
    const char *slash;
    size_t dir_len;

    if (self_path == 0 || tool_name == 0 || out == 0 || out_size == 0u) {
        return -1;
    }

    slash = strrchr(self_path, '/');
    if (slash == 0) {
        return snprintf(out, out_size, "%s", tool_name) < (int)out_size ? 0 : -1;
    }

    dir_len = (size_t)(slash - self_path);
    if (dir_len + 1u + strlen(tool_name) + 1u > out_size) {
        return -1;
    }

    memcpy(out, self_path, dir_len);
    out[dir_len] = '/';
    strcpy(out + dir_len + 1u, tool_name);
    return 0;
}

static scanned_service_t *find_or_add_scanned_service(scanned_service_t *services,
                                                      size_t capacity,
                                                      size_t *count,
                                                      uint32_t sid) {
    size_t i;

    if (services == 0 || count == 0) {
        return 0;
    }

    for (i = 0u; i < *count; ++i) {
        if (services[i].used != 0 && services[i].sid == sid) {
            return &services[i];
        }
    }

    if (*count >= capacity) {
        return 0;
    }

    services[*count] = (scanned_service_t){0};
    services[*count].used = 1;
    services[*count].sid = sid;
    (*count)++;
    return &services[*count - 1u];
}

static void maybe_store_service_line(scanned_service_t *services,
                                     size_t capacity,
                                     size_t *count,
                                     const char *line) {
    const char *sid_pos;
    const char *label_pos;
    unsigned int sid = 0u;
    scanned_service_t *service;

    if (line == 0 || strncmp(line, " fic_service sid=0x", 18) != 0) {
        return;
    }

    sid_pos = strstr(line, "sid=0x");
    label_pos = strstr(line, " label=\"");
    if (sid_pos == 0 || label_pos == 0) {
        return;
    }
    if (sscanf(sid_pos, "sid=0x%x", &sid) != 1) {
        return;
    }

    service = find_or_add_scanned_service(services, capacity, count, (uint32_t)sid);
    if (service == 0) {
        return;
    }

    label_pos += 8;
    {
        const char *label_end = strchr(label_pos, '"');
        size_t length;

        if (label_end == 0) {
            return;
        }
        length = (size_t)(label_end - label_pos);
        if (length >= sizeof(service->label)) {
            length = sizeof(service->label) - 1u;
        }
        memcpy(service->label, label_pos, length);
        service->label[length] = '\0';
    }
}

static int write_scanned_services(FILE *fp,
                                  const dab_channel_t *channel,
                                  const scanned_service_t *services,
                                  size_t count) {
    size_t i;

    if (fp == 0 || channel == 0 || services == 0) {
        return -1;
    }

    for (i = 0u; i < count; ++i) {
        if (services[i].used == 0) {
            continue;
        }
        if (fprintf(fp,
                    "%s\t%llu\t0x%08x\t%s\n",
                    channel->channel,
                    (unsigned long long)channel->frequency_hz,
                    services[i].sid,
                    services[i].label) < 0) {
            return -1;
        }
    }

    return fflush(fp) == 0 ? 0 : -1;
}

static int scan_frequency_via_audio_tool(const char *argv0,
                                         const dab_channel_t *channel,
                                         uint32_t channel_index,
                                         FILE *results_fp) {
    char tool_path[1024];
    char freq_arg[32];
    int pipe_fds[2] = {-1, -1};
    pid_t pid;
    scanned_service_t services[64];
    size_t service_count = 0u;
    int service_total = -1;
    int label_total = -1;
    int saw_lock = 0;
    int rc = -1;
    time_t deadline;
    time_t lock_deadline = 0;
    char line_buffer[2048];
    size_t line_length = 0u;

    memset(services, 0, sizeof(services));

    if (build_sibling_tool_path(argv0, "dab_sdrplay_audio", tool_path, sizeof(tool_path)) != 0) {
        fprintf(stderr, "failed to resolve dab_sdrplay_audio path\n");
        return -1;
    }

    snprintf(freq_arg, sizeof(freq_arg), "%llu", (unsigned long long)channel->frequency_hz);
    printf("SCAN_PROGRESS\t%u\t%d\t%s\t%llu\n",
           channel_index + 1u,
           DAB_CHANNEL_COUNT,
           channel->channel,
           (unsigned long long)channel->frequency_hz);
    fflush(stdout);

    if (pipe(pipe_fds) != 0) {
        fprintf(stderr, "pipe failed for %s\n", channel->channel);
        return -1;
    }

    pid = fork();
    if (pid < 0) {
        fprintf(stderr, "fork failed for %s\n", channel->channel);
        close(pipe_fds[0]);
        close(pipe_fds[1]);
        return -1;
    }

    if (pid == 0) {
        dup2(pipe_fds[1], STDOUT_FILENO);
        dup2(pipe_fds[1], STDERR_FILENO);
        close(pipe_fds[0]);
        close(pipe_fds[1]);
        setenv("DAB_SDRPLAY_USE_REAL", "1", 1);
        execl(tool_path,
              tool_path,
              "--freq",
              freq_arg,
              "--agc",
              "on",
              "--debug",
              "on",
              (char *)0);
        _exit(127);
    }

    close(pipe_fds[1]);
    pipe_fds[1] = -1;
    deadline = time(0) + 4;

    while (time(0) < deadline) {
        fd_set readfds;
        struct timeval timeout;
        int ready;
        char chunk[256];
        ssize_t bytes_read;

        FD_ZERO(&readfds);
        FD_SET(pipe_fds[0], &readfds);
        timeout.tv_sec = 0;
        timeout.tv_usec = 200000;
        ready = select(pipe_fds[0] + 1, &readfds, 0, 0, &timeout);
        if (ready < 0) {
            break;
        }
        if (ready == 0) {
            if (service_total > 0 && label_total >= service_total) {
                break;
            }
            if (saw_lock && lock_deadline != 0 && time(0) >= lock_deadline) {
                break;
            }
            continue;
        }

        bytes_read = read(pipe_fds[0], chunk, sizeof(chunk));
        if (bytes_read <= 0) {
            break;
        }

        for (ssize_t i = 0; i < bytes_read; ++i) {
            if (chunk[i] == '\n' || line_length + 1u >= sizeof(line_buffer)) {
                line_buffer[line_length] = '\0';
                if (strncmp(line_buffer, "fic_repo ", 9) == 0) {
                    int parsed_services = -1;
                    int parsed_labels = -1;
                    if (sscanf(line_buffer, "fic_repo eid=%*s subchannels=%*u services=%d labels=%d",
                               &parsed_services,
                               &parsed_labels) == 2) {
                        service_total = parsed_services;
                        label_total = parsed_labels;
                        if (parsed_services > 0 && !saw_lock) {
                            saw_lock = 1;
                            lock_deadline = time(0) + 10;
                            deadline = lock_deadline;
                            printf("SCAN_LOCK\t%u\t%d\t%s\t%llu\t%zu\t%d\n",
                                   channel_index + 1u,
                                   DAB_CHANNEL_COUNT,
                                   channel->channel,
                                   (unsigned long long)channel->frequency_hz,
                                   service_count,
                                   parsed_labels);
                            fflush(stdout);
                        }
                    }
                }
                if (strncmp(line_buffer, " fic_service sid=0x", 18) == 0) {
                    maybe_store_service_line(services, sizeof(services) / sizeof(services[0]), &service_count, line_buffer);
                    if (!saw_lock) {
                        saw_lock = 1;
                        lock_deadline = time(0) + 10;
                        deadline = lock_deadline;
                        printf("SCAN_LOCK\t%u\t%d\t%s\t%llu\t%zu\t%d\n",
                               channel_index + 1u,
                               DAB_CHANNEL_COUNT,
                               channel->channel,
                               (unsigned long long)channel->frequency_hz,
                               service_count,
                               label_total >= 0 ? label_total : 0);
                        fflush(stdout);
                    }
                }
                line_length = 0u;
                continue;
            }
            line_buffer[line_length++] = chunk[i];
        }
    }

    if (kill(pid, SIGTERM) != 0) {
        (void)kill(pid, SIGKILL);
    }
    {
        int status = 0;
        int wait_tries = 0;
        while (waitpid(pid, &status, WNOHANG) == 0 && wait_tries < 30) {
            usleep(100000);
            wait_tries++;
        }
        if (wait_tries >= 30) {
            (void)kill(pid, SIGKILL);
            (void)waitpid(pid, &status, 0);
        }
    }

    close(pipe_fds[0]);

    if (service_count > 0u) {
        if (write_scanned_services(results_fp, channel, services, service_count) != 0) {
            fprintf(stderr, "failed to write results for %s\n", channel->channel);
            return -1;
        }
        rc = (int)service_count;
    } else {
        rc = 0;
    }

    return rc;
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

static const dab_fic_label_t *fic_repo_find_best_service_label(const dab_fic_database_t *db,
                                                               uint8_t is_programme,
                                                               uint32_t sid) {
    uint16_t i;
    const dab_fic_label_t *label;

    label = fic_repo_find_label(db, DAB_FIC_LABEL_SCOPE_SERVICE, is_programme, sid, 0u, 0u);
    if (label != 0 && label->text[0] != '\0') {
        return label;
    }

    if (db == 0) {
        return 0;
    }

    for (i = 0u; i < db->label_count; ++i) {
        const dab_fic_label_t *candidate = &db->labels[i];

        if (candidate->used == 0u ||
            candidate->scope != DAB_FIC_LABEL_SCOPE_COMPONENT ||
            candidate->is_programme != is_programme ||
            candidate->sid != sid ||
            candidate->text[0] == '\0') {
            continue;
        }
        return candidate;
    }

    return 0;
}

static uint32_t fic_repo_service_label_count(const dab_fic_database_t *db) {
    uint16_t i;
    uint32_t count = 0u;

    if (db == 0) {
        return 0u;
    }

    for (i = 0u; i < db->service_count; ++i) {
        const dab_fic_service_t *service = &db->services[i];
        if (fic_repo_find_best_service_label(db, service->is_programme, service->sid) != 0) {
            count++;
        }
    }
    return count;
}

static int write_scan_results(FILE *fp, const dab_channel_t *channel, const dab_fic_database_t *db) {
    uint16_t i;

    if (fp == 0 || channel == 0 || db == 0) {
        return -1;
    }

    for (i = 0u; i < db->service_count; ++i) {
        const dab_fic_service_t *service = &db->services[i];
        const dab_fic_label_t *label = fic_repo_find_best_service_label(db, service->is_programme, service->sid);
        const char *name = (label != 0 && label->text[0] != '\0') ? label->text : "";

        if (fprintf(fp,
                    "%s\t%llu\t0x%08x\t%s\n",
                    channel->channel,
                    (unsigned long long)channel->frequency_hz,
                    service->sid,
                    name) < 0) {
            return -1;
        }
    }

    return fflush(fp) == 0 ? 0 : -1;
}

static uint64_t apply_prs_fine_timing(uint64_t fft_start_sample,
                                      float fine_timing_error_samples,
                                      float max_abs_timing_error_samples) {
    const float abs_timing = fabsf(fine_timing_error_samples);
    const int32_t correction = (int32_t)lroundf(fine_timing_error_samples);

    if (!(abs_timing > 0.0f) || abs_timing > max_abs_timing_error_samples || correction == 0) {
        return fft_start_sample;
    }
    if (correction < 0) {
        return fft_start_sample + (uint64_t)(-correction);
    }
    if (fft_start_sample < (uint64_t)correction) {
        return fft_start_sample;
    }
    return fft_start_sample - (uint64_t)correction;
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
    *history = (history_buffer_t){0};
}

static void history_reset(history_buffer_t *history) {
    if (history == 0) {
        return;
    }
    history->sample_count = 0u;
    history->start_sample_index = 0u;
}

static void history_append(history_buffer_t *history, const dab_sdr_block_view_t *block) {
    uint32_t drop;
    uint32_t keep;
    uint64_t expected_next;

    if (history == 0 || block == 0 || history->iq == 0 || block->iq_interleaved == 0) {
        return;
    }
    if (history->sample_count == 0u) {
        keep = block->sample_count > history->capacity_samples ? history->capacity_samples : block->sample_count;
        memcpy(history->iq,
               block->iq_interleaved + (2u * (block->sample_count - keep)),
               sizeof(*history->iq) * 2u * keep);
        history->sample_count = keep;
        history->start_sample_index = block->sample_index + (block->sample_count - keep);
        return;
    }

    expected_next = history->start_sample_index + history->sample_count;
    if (block->sample_index != expected_next) {
        history_reset(history);
        history_append(history, block);
        return;
    }

    keep = block->sample_count;
    if (keep > history->capacity_samples) {
        history_reset(history);
        history_append(history, block);
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
    return view;
}

static int history_contains_sample_range(const history_buffer_t *history,
                                         uint64_t start_sample,
                                         uint64_t end_sample) {
    const uint64_t history_end =
        history != 0 ? history->start_sample_index + history->sample_count : 0u;

    return history != 0 &&
           history->sample_count != 0u &&
           start_sample >= history->start_sample_index &&
           end_sample <= history_end;
}

static void scan_runtime_reset(scan_runtime_t *runtime) {
    if (runtime == 0) {
        return;
    }

    dab_null_detect_reset(&runtime->detector);
    dab_ofdm_reset(&runtime->ofdm);
    dab_sync_reset(&runtime->sync);
    dab_tframe_reset(&runtime->tframe);
    dab_fic_reset(&runtime->fic);
    history_reset(&runtime->history);
    runtime->pending = (pending_candidate_t){0};
    runtime->last_predicted_fft_start = 0u;
}

static int scan_runtime_init(scan_runtime_t *runtime, uint32_t sample_rate) {
    dab_null_detect_config_t detector_config;
    dab_prs_acquire_config_t acquire_config;
    dab_prs_score_config_t score_config;
    dab_prs_fft_config_t fft_config;
    dab_ofdm_config_t ofdm_config;
    dab_sync_config_t sync_config;

    if (runtime == 0) {
        return -1;
    }

    *runtime = (scan_runtime_t){0};
    runtime->ref_threshold = 0.10f;
    runtime->prs_fine_timing_limit = 32.0f;

    dab_null_detect_default_config(&detector_config);
    detector_config.fast_window_samples = 128u;
    detector_config.slow_window_samples = 8192u;
    detector_config.sample_decimation = 4u;
    detector_config.enter_ratio_threshold = 0.35f;
    detector_config.exit_ratio_threshold = 0.55f;
    detector_config.min_null_samples = 1800u;
    detector_config.max_null_samples = 4000u;
    if (dab_null_detect_init(&runtime->detector, &detector_config) != 0) {
        return -1;
    }

    dab_prs_acquire_default_config(&acquire_config);
    if (dab_prs_acquire_init(&runtime->acquire, &acquire_config) != 0) {
        return -1;
    }
    runtime->symbol_samples = acquire_config.symbol_samples;
    runtime->fft_samples = acquire_config.fft_samples;

    dab_prs_score_default_config(&score_config);
    score_config.sample_rate = sample_rate;
    score_config.guard_samples = acquire_config.guard_samples;
    score_config.fft_samples = acquire_config.fft_samples;
    if (dab_prs_score_init(&runtime->cp_score, &score_config) != 0) {
        return -1;
    }

    dab_prs_fft_default_config(&fft_config);
    fft_config.sample_rate = sample_rate;
    fft_config.fft_samples = acquire_config.fft_samples;
    if (dab_prs_fft_init(&runtime->fft, &fft_config) != 0) {
        return -1;
    }

    dab_sync_default_config(&sync_config);
    sync_config.acquire_timing_window_samples = acquire_config.acquire_window_samples;
    sync_config.acquire_prs_score_threshold = runtime->ref_threshold;
    sync_config.track_prs_score_threshold = runtime->ref_threshold * 0.8f;
    if (dab_sync_init(&runtime->sync, &sync_config) != 0) {
        return -1;
    }

    dab_ofdm_default_config(&ofdm_config);
    if (dab_ofdm_init(&runtime->ofdm, &ofdm_config) != 0) {
        return -1;
    }

    runtime->fft_i = (float *)calloc(acquire_config.fft_samples, sizeof(*runtime->fft_i));
    runtime->fft_q = (float *)calloc(acquire_config.fft_samples, sizeof(*runtime->fft_q));
    runtime->active_i = (float *)calloc(DAB_MODE_I_ACTIVE_CARRIERS, sizeof(*runtime->active_i));
    runtime->active_q = (float *)calloc(DAB_MODE_I_ACTIVE_CARRIERS, sizeof(*runtime->active_q));
    runtime->prev_active_i = (float *)calloc(DAB_MODE_I_ACTIVE_CARRIERS, sizeof(*runtime->prev_active_i));
    runtime->prev_active_q = (float *)calloc(DAB_MODE_I_ACTIVE_CARRIERS, sizeof(*runtime->prev_active_q));
    runtime->diff_i = (float *)calloc(DAB_MODE_I_ACTIVE_CARRIERS, sizeof(*runtime->diff_i));
    runtime->diff_q = (float *)calloc(DAB_MODE_I_ACTIVE_CARRIERS, sizeof(*runtime->diff_q));
    runtime->deintl_i = (float *)calloc(DAB_MODE_I_DATA_CARRIERS, sizeof(*runtime->deintl_i));
    runtime->deintl_q = (float *)calloc(DAB_MODE_I_DATA_CARRIERS, sizeof(*runtime->deintl_q));
    runtime->soft_a = (int8_t *)calloc(DAB_MODE_I_DATA_CARRIERS, sizeof(*runtime->soft_a));
    runtime->soft_b = (int8_t *)calloc(DAB_MODE_I_DATA_CARRIERS, sizeof(*runtime->soft_b));
    runtime->fic_softbits = (int8_t *)calloc(DAB_TFRAME_FIC_SOFTBITS, sizeof(*runtime->fic_softbits));
    runtime->msc_softbits = (int8_t *)calloc(DAB_TFRAME_MSC_SOFTBITS, sizeof(*runtime->msc_softbits));
    runtime->frame_softbits = (int8_t *)calloc(DAB_TFRAME_TOTAL_SOFTBITS, sizeof(*runtime->frame_softbits));
    if (runtime->fft_i == 0 || runtime->fft_q == 0 ||
        runtime->active_i == 0 || runtime->active_q == 0 ||
        runtime->prev_active_i == 0 || runtime->prev_active_q == 0 ||
        runtime->diff_i == 0 || runtime->diff_q == 0 ||
        runtime->deintl_i == 0 || runtime->deintl_q == 0 ||
        runtime->soft_a == 0 || runtime->soft_b == 0 ||
        runtime->fic_softbits == 0 || runtime->msc_softbits == 0 || runtime->frame_softbits == 0) {
        return -1;
    }

    runtime->ofdm.prev_active_i = runtime->prev_active_i;
    runtime->ofdm.prev_active_q = runtime->prev_active_q;
    runtime->tframe_buffers.fic_softbits = runtime->fic_softbits;
    runtime->tframe_buffers.msc_softbits = runtime->msc_softbits;
    runtime->tframe_buffers.frame_softbits = runtime->frame_softbits;
    if (dab_tframe_init(&runtime->tframe, &runtime->tframe_buffers) != 0) {
        return -1;
    }
    if (dab_fic_init(&runtime->fic) != 0) {
        return -1;
    }
    if (history_init(&runtime->history, 262144u) != 0) {
        return -1;
    }

    return 0;
}

static void scan_runtime_destroy(scan_runtime_t *runtime) {
    if (runtime == 0) {
        return;
    }

    history_destroy(&runtime->history);
    free(runtime->fft_i);
    free(runtime->fft_q);
    free(runtime->active_i);
    free(runtime->active_q);
    free(runtime->prev_active_i);
    free(runtime->prev_active_q);
    free(runtime->diff_i);
    free(runtime->diff_q);
    free(runtime->deintl_i);
    free(runtime->deintl_q);
    free(runtime->soft_a);
    free(runtime->soft_b);
    free(runtime->fic_softbits);
    free(runtime->msc_softbits);
    free(runtime->frame_softbits);
    *runtime = (scan_runtime_t){0};
}

static int process_locked_frame(scan_runtime_t *runtime,
                                const dab_sdr_block_view_t *hist_view,
                                uint64_t prs_fft_start_sample,
                                float cfo_hz,
                                int32_t carrier_offset_bins) {
    dab_ofdm_buffers_t ofdm_buffers;
    dab_ofdm_result_t ofdm_result;
    uint32_t symbol_idx;

    ofdm_buffers.active_i = runtime->active_i;
    ofdm_buffers.active_q = runtime->active_q;
    ofdm_buffers.diff_i = runtime->diff_i;
    ofdm_buffers.diff_q = runtime->diff_q;
    ofdm_buffers.deintl_i = runtime->deintl_i;
    ofdm_buffers.deintl_q = runtime->deintl_q;
    ofdm_buffers.soft_a = runtime->soft_a;
    ofdm_buffers.soft_b = runtime->soft_b;

    dab_ofdm_reset(&runtime->ofdm);
    dab_tframe_reset(&runtime->tframe);
    (void)dab_ofdm_process_mode_i(&runtime->ofdm,
                                  runtime->fft_i,
                                  runtime->fft_q,
                                  prs_fft_start_sample,
                                  DAB_OFDM_SYMBOL_PRS,
                                  &ofdm_buffers,
                                  &ofdm_result);

    for (symbol_idx = 0u; symbol_idx < DAB_TFRAME_TOTAL_SYMBOLS; ++symbol_idx) {
        const uint64_t data_fft_start =
            prs_fft_start_sample + ((uint64_t)(symbol_idx + 1u) * runtime->symbol_samples);
        dab_tframe_result_t tframe_result;

        if (!history_contains_sample_range(&runtime->history,
                                           data_fft_start,
                                           data_fft_start + runtime->fft_samples)) {
            break;
        }
        if (dab_prs_fft_execute_i16_cfo(&runtime->fft,
                                        hist_view,
                                        data_fft_start,
                                        cfo_hz,
                                        runtime->fft_i,
                                        runtime->fft_q) != 0) {
            break;
        }
        if (carrier_offset_bins != 0) {
            (void)dab_prs_fft_apply_bin_shift(runtime->fft_i,
                                              runtime->fft_q,
                                              runtime->fft_samples,
                                              -carrier_offset_bins);
        }
        if (dab_ofdm_process_mode_i(&runtime->ofdm,
                                    runtime->fft_i,
                                    runtime->fft_q,
                                    data_fft_start,
                                    DAB_OFDM_SYMBOL_DATA,
                                    &ofdm_buffers,
                                    &ofdm_result) != 0) {
            break;
        }
        if (ofdm_result.have_softbits == 0) {
            continue;
        }
        if (dab_tframe_push_symbol(&runtime->tframe,
                                   data_fft_start,
                                   runtime->soft_a,
                                   runtime->soft_b,
                                   &tframe_result) != 0) {
            continue;
        }
        if (tframe_result.frame_complete &&
            dab_fic_process_frame(&runtime->fic,
                                  tframe_result.frame_start_sample,
                                  runtime->fic_softbits,
                                  DAB_TFRAME_FIC_SOFTBITS) == 0 &&
            runtime->fic.valid_fibs_last_frame > 0u) {
            return 1;
        }
    }

    return 0;
}

static int scan_frequency(scan_runtime_t *runtime,
                          dab_sdr_device_t *device,
                          const dab_sdrplay_config_t *device_config,
                          const dab_channel_t *channel,
                          uint32_t channel_index,
                          uint32_t per_frequency_seconds,
                          FILE *results_fp) {
    dab_stream_config_t stream_config;
    dab_sdr_stream_t *stream = 0;
    dab_sdr_block_view_t src_block;
    uint32_t block_index;
    uint32_t block_limit;
    uint32_t metadata_updated_block = 0u;
    uint32_t label_wait_block_limit = 0u;
    uint32_t best_service_count = 0u;
    uint32_t best_label_count = 0u;
    int have_services = 0;
    int rc = -1;

    if (runtime == 0 || device == 0 || device_config == 0 || channel == 0 || results_fp == 0) {
        return -1;
    }

    scan_runtime_reset(runtime);

    stream_config.sample_rate = 2048000u;
    stream_config.center_freq_hz = channel->frequency_hz;
    stream_config.bandwidth_hz = 1536000u;
    stream_config.gain_db = 40;
    block_limit = (per_frequency_seconds * stream_config.sample_rate) / 1334;//DAB_IQ_BLOCK_SAMPLES;
    if (block_limit < 120u) {
        block_limit = 120u;
    }
    label_wait_block_limit = (10u * stream_config.sample_rate) / 1334;//DAB_IQ_BLOCK_SAMPLES;
    if (label_wait_block_limit < 24u) {
        label_wait_block_limit = 24u;
    }

    printf("SCAN_PROGRESS\t%u\t%d\t%s\t%llu\n",
           channel_index + 1u,
           DAB_CHANNEL_COUNT,
           channel->channel,
           (unsigned long long)channel->frequency_hz);
    fflush(stdout);

    if (dab_sdrplay_configure_device(device, device_config) != DAB_SDR_STATUS_OK) {
        fprintf(stderr, "device config failed for %s\n", channel->channel);
        return -1;
    }
    if (device->vtbl->configure(device, &stream_config) != DAB_SDR_STATUS_OK) {
        fprintf(stderr, "stream config failed for %s\n", channel->channel);
        return -1;
    }
    if (device->vtbl->open_stream(device, &stream) != DAB_SDR_STATUS_OK || stream == 0) {
        fprintf(stderr, "open stream failed for %s\n", channel->channel);
        return -1;
    }
    if (stream->vtbl->start(stream) != DAB_SDR_STATUS_OK) {
        fprintf(stderr, "start stream failed for %s\n", channel->channel);
        dab_sdr_stream_close(stream);
        return -1;
    }

    for (block_index = 0u; block_index < block_limit; ++block_index) {
        dab_sdr_block_view_t hist_view;
        dab_null_event_t event = {0};

        if (stream->vtbl->read(stream, &src_block) != DAB_SDR_STATUS_OK) {
            fprintf(stderr, "read failed for %s at block %u\n", channel->channel, block_index);
            goto cleanup;
        }

        history_append(&runtime->history, &src_block);
        if (dab_null_detect_process_i16(&runtime->detector, &src_block, &event) < 0) {
            fprintf(stderr, "null detector failed for %s\n", channel->channel);
            goto cleanup;
        }

        if (event.length != 0u && !runtime->sync.state.locked) {
            if (dab_prs_candidate_from_null(&runtime->acquire, &event, &runtime->pending.candidate) == 0) {
                runtime->pending.pending = 1;
                runtime->pending.predicted = 0;
                runtime->pending.event = event;
            }
        }

        if (!runtime->pending.pending && runtime->sync.state.locked) {
            dab_prs_candidate_t predicted_candidate = {0};

            init_predicted_candidate(&runtime->acquire, &runtime->sync, &predicted_candidate);
            if (predicted_candidate.prs_fft_start_sample != runtime->last_predicted_fft_start) {
                runtime->pending.pending = 1;
                runtime->pending.predicted = 1;
                runtime->pending.candidate = predicted_candidate;
                runtime->pending.event = (dab_null_event_t){0};
                runtime->last_predicted_fft_start = predicted_candidate.prs_fft_start_sample;
            }
        }

        if (!runtime->pending.pending) {
            if (have_services && block_index >= metadata_updated_block + label_wait_block_limit) {
                break;
            }
            continue;
        }

        {
            uint64_t required_end =
                dab_prs_candidate_search_end_sample(&runtime->pending.candidate) + runtime->fft_samples;
            if (runtime->sync.state.locked) {
                required_end += ((uint64_t)DAB_TFRAME_TOTAL_SYMBOLS * runtime->symbol_samples);
            }
            if (!history_contains_sample_range(&runtime->history,
                                               dab_prs_candidate_search_start_sample(&runtime->pending.candidate),
                                               required_end)) {
                continue;
            }
        }

        hist_view = history_view(&runtime->history);
        {
            dab_prs_result_t coarse = {0};
            dab_prs_reference_score_t ref_score = {0};
            const dab_null_event_t pending_event = runtime->pending.event;
            float cfo_for_fft = coarse.coarse_freq_error_hz;
            const int coarse_rc =
                dab_prs_score_search_i16(&runtime->cp_score, &hist_view, &runtime->pending.candidate, &coarse);

            runtime->pending.pending = 0;
            runtime->pending.predicted = 0;

            if (coarse_rc != 1) {
                continue;
            }

            cfo_for_fft = coarse.coarse_freq_error_hz;
            if (runtime->sync.state.locked && runtime->sync.state.bad_frames == 0u) {
                cfo_for_fft = runtime->sync.state.coarse_freq_error_hz;
            }

            if (dab_prs_score_symbol_i16_mode_i_diff_cfo(&runtime->fft,
                                                         &hist_view,
                                                         coarse.prs_fft_start_sample,
                                                         cfo_for_fft,
                                                         runtime->fft_i,
                                                         runtime->fft_q,
                                                         &ref_score) != 0) {
                continue;
            }

            {
                const uint64_t prs_fft_start_sample =
                    apply_prs_fine_timing(coarse.prs_fft_start_sample,
                                          -ref_score.fine_timing_error_samples,
                                          runtime->prs_fine_timing_limit);
                int sync_rc;

                if (prs_fft_start_sample != coarse.prs_fft_start_sample &&
                    dab_prs_score_symbol_i16_mode_i_diff_cfo(&runtime->fft,
                                                             &hist_view,
                                                             prs_fft_start_sample,
                                                             cfo_for_fft,
                                                             runtime->fft_i,
                                                             runtime->fft_q,
                                                             &ref_score) != 0) {
                    continue;
                }

                if (ref_score.carrier_offset_bins != 0) {
                    (void)dab_prs_fft_apply_bin_shift(runtime->fft_i,
                                                      runtime->fft_q,
                                                      runtime->fft_samples,
                                                      -ref_score.carrier_offset_bins);
                }
                if (dab_ofdm_extract_active_carriers_mode_i(runtime->fft_i,
                                                            runtime->fft_q,
                                                            runtime->active_i,
                                                            runtime->active_q) != 0) {
                    continue;
                }

                if (runtime->sync.state.locked) {
                    sync_rc = dab_sync_on_prs_track(&runtime->sync,
                                                    prs_fft_start_sample,
                                                    ref_score.score,
                                                    coarse.coarse_freq_error_hz,
                                                    ref_score.carrier_offset_bins);
                } else {
                    dab_sync_on_null_candidate(&runtime->sync, &pending_event);
                    sync_rc = dab_sync_on_prs_acquire(&runtime->sync,
                                                      prs_fft_start_sample,
                                                      ref_score.score,
                                                      coarse.coarse_freq_error_hz,
                                                      ref_score.carrier_offset_bins);
                }

                if (sync_rc < 0 || !runtime->sync.state.locked) {
                    runtime->last_predicted_fft_start = 0u;
                    dab_ofdm_reset(&runtime->ofdm);
                    dab_tframe_reset(&runtime->tframe);
                    continue;
                }
                if (ref_score.score < runtime->ref_threshold) {
                    continue;
                }

                if (process_locked_frame(runtime,
                                         &hist_view,
                                         prs_fft_start_sample,
                                         cfo_for_fft,
                                         ref_score.carrier_offset_bins) == 0) {
                    continue;
                }

                if (runtime->fic.repo.current.service_count > 0u) {
                    const uint32_t label_count = fic_repo_service_label_count(&runtime->fic.repo.current);
                    const uint32_t service_count = runtime->fic.repo.current.service_count;

                    if (service_count > best_service_count || label_count > best_label_count) {
                        metadata_updated_block = block_index;
                        if (service_count > best_service_count) {
                            best_service_count = service_count;
                        }
                        if (label_count > best_label_count) {
                            best_label_count = label_count;
                        }
                    }

                    printf("SCAN_LOCK\t%u\t%d\t%s\t%llu\t%u\t%u\n",
                           channel_index + 1u,
                           DAB_CHANNEL_COUNT,
                           channel->channel,
                           (unsigned long long)channel->frequency_hz,
                           service_count,
                           label_count);
                    fflush(stdout);
                    have_services = 1;
                    if (label_count >= service_count ||
                        block_index >= metadata_updated_block + label_wait_block_limit) {
                        break;
                    }
                }
            }
        }
    }

    if (have_services) {
        if (write_scan_results(results_fp, channel, &runtime->fic.repo.current) != 0) {
            fprintf(stderr, "failed to write scan results for %s\n", channel->channel);
            goto cleanup;
        }
        rc = (int)runtime->fic.repo.current.service_count;
    } else {
        rc = 0;
    }

cleanup:
    if (stream != 0) {
        dab_sdr_stream_close(stream);
    }
    return rc;
}

int main(int argc, char **argv) {
    dab_sdr_open_params_t params = {"sdrplay", NULL, NULL};
    dab_sdrplay_config_t device_config;
    dab_sdr_device_t *device = 0;
    scan_runtime_t runtime = {0};
    const char *output_path = 0;
    uint32_t per_frequency_seconds = 4u;
    uint32_t channel_index;
    uint32_t total_services = 0u;
    FILE *results_fp = 0;
    int rc = 1;

    setvbuf(stdout, NULL, _IOLBF, 0);
    setvbuf(stderr, NULL, _IOLBF, 0);

    dab_sdrplay_default_config(&device_config);
    device_config.gain_mode = DAB_SDRPLAY_GAIN_MODE_AUTO;
    device_config.if_mode = DAB_SDRPLAY_IF_MODE_ZERO_IF;
    device_config.antenna = DAB_SDRPLAY_ANTENNA_A;

    for (channel_index = 1u; channel_index < (uint32_t)argc; ++channel_index) {
        if (strcmp(argv[channel_index], "--output") == 0) {
            if (channel_index + 1u >= (uint32_t)argc) {
                print_usage(argv[0]);
                return 2;
            }
            output_path = argv[++channel_index];
            continue;
        }
        if (strcmp(argv[channel_index], "--agc") == 0) {
            if (channel_index + 1u >= (uint32_t)argc ||
                parse_agc_arg(argv[++channel_index], &device_config.gain_mode) != 0) {
                print_usage(argv[0]);
                return 2;
            }
            continue;
        }
        if (strcmp(argv[channel_index], "--per-frequency-seconds") == 0) {
            if (channel_index + 1u >= (uint32_t)argc ||
                parse_u32_arg(argv[++channel_index], &per_frequency_seconds) != 0) {
                print_usage(argv[0]);
                return 2;
            }
            continue;
        }
        if (strcmp(argv[channel_index], "--help") == 0 || strcmp(argv[channel_index], "-h") == 0) {
            print_usage(argv[0]);
            return 0;
        }

        print_usage(argv[0]);
        return 2;
    }

    if (output_path == 0 || output_path[0] == '\0') {
        print_usage(argv[0]);
        return 2;
    }

    results_fp = fopen(output_path, "wb");
    if (results_fp == 0) {
        fprintf(stderr, "failed to open output file: %s\n", output_path);
        return 2;
    }
    if (fprintf(results_fp, "channel\tfrequency_hz\tservice_id\tlabel\n") < 0) {
        fprintf(stderr, "failed to write output header\n");
        goto cleanup;
    }

    if (scan_runtime_init(&runtime, 2048000u) != 0) {
        fprintf(stderr, "failed to initialize scan runtime\n");
        goto cleanup;
    }
    if (dab_sdr_device_open(&params, &device) != DAB_SDR_STATUS_OK || device == 0) {
        fprintf(stderr, "failed to open SDRplay device\n");
        goto cleanup;
    }

    for (channel_index = 0u; channel_index < (uint32_t)DAB_CHANNEL_COUNT; ++channel_index) {
        const int services =
            scan_frequency(&runtime,
                           device,
                           &device_config,
                           &kBandIIIChannels[channel_index],
                           channel_index,
                           per_frequency_seconds,
                           results_fp);
        if (services < 0) {
            printf("SCAN_ERROR\tchannel %s failed\n", kBandIIIChannels[channel_index].channel);
            fflush(stdout);
            goto cleanup;
        }
        total_services += (uint32_t)services;
    }

    printf("SCAN_COMPLETE\t%s\t%u\n", output_path, total_services);
    rc = 0;
cleanup:
    if (device != 0) {
        dab_sdr_device_close(device);
    }
    scan_runtime_destroy(&runtime);
    if (results_fp != 0) {
        fclose(results_fp);
    }
    return rc;
}
