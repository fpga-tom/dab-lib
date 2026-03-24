#ifndef DAB_OS_MUTEX_H
#define DAB_OS_MUTEX_H

#if defined(_WIN32)
#include <windows.h>
typedef struct dab_mutex {
    CRITICAL_SECTION handle;
} dab_mutex_t;
#else
#include <pthread.h>
typedef struct dab_mutex {
    pthread_mutex_t handle;
} dab_mutex_t;
#endif

int dab_mutex_init(dab_mutex_t *mutex);
void dab_mutex_destroy(dab_mutex_t *mutex);
void dab_mutex_lock(dab_mutex_t *mutex);
void dab_mutex_unlock(dab_mutex_t *mutex);

#endif
