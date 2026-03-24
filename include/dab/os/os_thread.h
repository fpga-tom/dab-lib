#ifndef DAB_OS_THREAD_H
#define DAB_OS_THREAD_H

#if defined(_WIN32)
#include <windows.h>
#else
#include <pthread.h>
#endif

typedef int (*dab_thread_entry_t)(void *arg);

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

#endif
