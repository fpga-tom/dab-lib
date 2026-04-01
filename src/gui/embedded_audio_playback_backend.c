#include "dab_gui_backend.h"

#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    pthread_t thread;
    int thread_valid;
    int running;
    uint64_t frequency_hz;
    uint32_t sid;
} dab_gui_playback_state_t;

static dab_gui_playback_state_t g_playback_state = {0};
static dab_gui_playback_status_t g_playback_status = {0};
static pthread_mutex_t g_playback_mutex = PTHREAD_MUTEX_INITIALIZER;

#define DAB_GUI_PLAYBACK_EMBEDDED 1

void dab_gui_embedded_audio_status_reset(void) {
    (void)pthread_mutex_lock(&g_playback_mutex);
    g_playback_status.locked = 0;
    g_playback_status.has_receiver_snr = 0;
    g_playback_status.receiver_snr_db = 0.0f;
    g_playback_status.has_spectrum_cn = 0;
    g_playback_status.spectrum_cn_db = 0.0f;
    g_playback_status.prs_score = 0.0f;
    (void)pthread_mutex_unlock(&g_playback_mutex);
}

void dab_gui_embedded_audio_status_update(int locked,
                                          int has_receiver_snr,
                                          float receiver_snr_db,
                                          int has_spectrum_cn,
                                          float spectrum_cn_db,
                                          float prs_score) {
    (void)pthread_mutex_lock(&g_playback_mutex);
    g_playback_status.locked = locked;
    g_playback_status.has_receiver_snr = has_receiver_snr;
    g_playback_status.receiver_snr_db = receiver_snr_db;
    g_playback_status.has_spectrum_cn = has_spectrum_cn;
    g_playback_status.spectrum_cn_db = spectrum_cn_db;
    g_playback_status.prs_score = prs_score;
    (void)pthread_mutex_unlock(&g_playback_mutex);
}

static void (*dab_gui_embedded_audio_signal(int signum, void (*handler)(int)))(int) {
    (void)signum;
    return handler;
}

#define signal dab_gui_embedded_audio_signal
#define main dab_gui_embedded_audio_main
#include "../../tools/dab_sdrplay_audio.c"
#undef main
#undef signal

static void *dab_gui_playback_thread_main(void *arg) {
    dab_gui_playback_state_t *state = (dab_gui_playback_state_t *)arg;
    char freq_arg[32];
    char sid_arg[32];
    char *argv[9];

    snprintf(freq_arg, sizeof(freq_arg), "%llu", (unsigned long long)state->frequency_hz);
    snprintf(sid_arg, sizeof(sid_arg), "0x%08x", state->sid);

    argv[0] = "dab_gui_embedded_audio";
    argv[1] = "--freq";
    argv[2] = freq_arg;
    argv[3] = "--agc";
    argv[4] = "on";
    argv[5] = "--debug";
    argv[6] = "off";
    argv[7] = "--sid";
    argv[8] = sid_arg;

    setenv("DAB_SDRPLAY_USE_REAL", "1", 1);
    g_stop_requested = 0;
    dab_gui_embedded_audio_status_reset();
    (void)dab_gui_embedded_audio_main(9, argv);

    (void)pthread_mutex_lock(&g_playback_mutex);
    g_playback_state.running = 0;
    g_playback_status.running = 0;
    g_playback_status.frequency_hz = g_playback_state.frequency_hz;
    g_playback_status.sid = g_playback_state.sid;
    (void)pthread_mutex_unlock(&g_playback_mutex);
    return 0;
}

int dab_gui_playback_start(uint64_t frequency_hz, uint32_t sid) {
    int rc = -1;
    pthread_attr_t attr;
    int attr_ready = 0;

    (void)pthread_mutex_lock(&g_playback_mutex);
    if (!g_playback_state.running) {
        if (g_playback_state.thread_valid) {
            (void)pthread_join(g_playback_state.thread, 0);
            g_playback_state.thread_valid = 0;
        }
        g_playback_state.frequency_hz = frequency_hz;
        g_playback_state.sid = sid;
        g_playback_state.running = 1;
        g_playback_status = (dab_gui_playback_status_t){
            .running = 1,
            .locked = 0,
            .has_receiver_snr = 0,
            .receiver_snr_db = 0.0f,
            .has_spectrum_cn = 0,
            .spectrum_cn_db = 0.0f,
            .prs_score = 0.0f,
            .frequency_hz = frequency_hz,
            .sid = sid,
        };
        if (pthread_attr_init(&attr) == 0) {
            attr_ready = 1;
            (void)pthread_attr_setstacksize(&attr, 8u * 1024u * 1024u);
        }
        rc = pthread_create(&g_playback_state.thread,
                            attr_ready ? &attr : 0,
                            dab_gui_playback_thread_main,
                            &g_playback_state) == 0 ? 0 : -1;
        if (attr_ready) {
            (void)pthread_attr_destroy(&attr);
        }
        if (rc != 0) {
            g_playback_state.running = 0;
        } else {
            g_playback_state.thread_valid = 1;
        }
    }
    (void)pthread_mutex_unlock(&g_playback_mutex);

    return rc;
}

void dab_gui_playback_stop(void) {
    int should_join = 0;

    (void)pthread_mutex_lock(&g_playback_mutex);
    if (g_playback_state.running) {
        g_stop_requested = 1;
        should_join = 1;
    }
    (void)pthread_mutex_unlock(&g_playback_mutex);

    if (should_join) {
        (void)pthread_join(g_playback_state.thread, 0);
        (void)pthread_mutex_lock(&g_playback_mutex);
        g_playback_state.thread_valid = 0;
        g_playback_state.running = 0;
        g_playback_status.running = 0;
        g_playback_status.locked = 0;
        g_playback_status.has_receiver_snr = 0;
        g_playback_status.has_spectrum_cn = 0;
        (void)pthread_mutex_unlock(&g_playback_mutex);
    }
}

int dab_gui_playback_is_running(void) {
    int running;

    (void)pthread_mutex_lock(&g_playback_mutex);
    running = g_playback_state.running;
    (void)pthread_mutex_unlock(&g_playback_mutex);
    return running;
}

int dab_gui_playback_get_status(dab_gui_playback_status_t *status) {
    if (status == 0) {
        return -1;
    }

    (void)pthread_mutex_lock(&g_playback_mutex);
    *status = g_playback_status;
    (void)pthread_mutex_unlock(&g_playback_mutex);
    return 0;
}
