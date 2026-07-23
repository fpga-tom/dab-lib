#ifndef DAB_OS_THREAD_H
#define DAB_OS_THREAD_H

#if defined(_WIN32)
#include <windows.h>
#else
#include <pthread.h>
#endif

typedef int (*dab_thread_entry_t)(void *arg);

typedef struct dab_thread_realtime_audio_scope {
    void *platform_handle;
    int active;
} dab_thread_realtime_audio_scope_t;

#if defined(_WIN32)
typedef struct dab_thread {
    HANDLE handle;
} dab_thread_t;
#else
typedef struct dab_thread {
    pthread_t handle;
} dab_thread_t;
#endif

int dab_thread_create(dab_thread_t *thread, dab_thread_entry_t entry, void *arg, const char *name);
int dab_thread_join(dab_thread_t *thread, int *result);
int dab_thread_realtime_audio_begin(dab_thread_realtime_audio_scope_t *scope);
void dab_thread_realtime_audio_end(dab_thread_realtime_audio_scope_t *scope);

#endif
