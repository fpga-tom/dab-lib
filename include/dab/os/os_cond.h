#ifndef DAB_OS_COND_H
#define DAB_OS_COND_H

#include <stdint.h>

#include "dab/os/os_mutex.h"

#if defined(_WIN32)
#include <windows.h>
typedef struct dab_cond {
    CONDITION_VARIABLE handle;
} dab_cond_t;
#else
#include <pthread.h>
typedef struct dab_cond {
    pthread_cond_t handle;
} dab_cond_t;
#endif

int dab_cond_init(dab_cond_t *cond);
void dab_cond_destroy(dab_cond_t *cond);
void dab_cond_wait(dab_cond_t *cond, dab_mutex_t *mutex);
/* Returns 0 when signalled, 1 on timeout, and -1 on error. */
int dab_cond_wait_timeout(dab_cond_t *cond, dab_mutex_t *mutex, uint32_t timeout_ms);
void dab_cond_signal(dab_cond_t *cond);
void dab_cond_broadcast(dab_cond_t *cond);

#endif
