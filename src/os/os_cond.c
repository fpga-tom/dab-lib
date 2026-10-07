#include "dab/os/os_cond.h"

#if defined(_WIN32)
#include <windows.h>

int dab_cond_init(dab_cond_t *cond) {
    InitializeConditionVariable(&cond->handle);
    return 0;
}

void dab_cond_destroy(dab_cond_t *cond) {
    (void)cond;
}

void dab_cond_wait(dab_cond_t *cond, dab_mutex_t *mutex) {
    SleepConditionVariableCS(&cond->handle, &mutex->handle, INFINITE);
}

int dab_cond_wait_timeout(dab_cond_t *cond, dab_mutex_t *mutex, uint32_t timeout_ms) {
    if (SleepConditionVariableCS(&cond->handle, &mutex->handle, (DWORD)timeout_ms)) {
        return 0;
    }
    return GetLastError() == ERROR_TIMEOUT ? 1 : -1;
}

void dab_cond_signal(dab_cond_t *cond) {
    WakeConditionVariable(&cond->handle);
}

void dab_cond_broadcast(dab_cond_t *cond) {
    WakeAllConditionVariable(&cond->handle);
}
#else
#include <errno.h>
#include <pthread.h>
#include <time.h>

int dab_cond_init(dab_cond_t *cond) {
    return pthread_cond_init(&cond->handle, 0);
}

void dab_cond_destroy(dab_cond_t *cond) {
    (void)pthread_cond_destroy(&cond->handle);
}

void dab_cond_wait(dab_cond_t *cond, dab_mutex_t *mutex) {
    (void)pthread_cond_wait(&cond->handle, &mutex->handle);
}

int dab_cond_wait_timeout(dab_cond_t *cond, dab_mutex_t *mutex, uint32_t timeout_ms) {
    struct timespec deadline;
    int rc;

    if (timespec_get(&deadline, TIME_UTC) != TIME_UTC) {
        return -1;
    }
    deadline.tv_sec += (time_t)(timeout_ms / 1000u);
    deadline.tv_nsec += (long)(timeout_ms % 1000u) * 1000000l;
    if (deadline.tv_nsec >= 1000000000l) {
        deadline.tv_sec++;
        deadline.tv_nsec -= 1000000000l;
    }

    rc = pthread_cond_timedwait(&cond->handle, &mutex->handle, &deadline);
    if (rc == 0) {
        return 0;
    }
    return rc == ETIMEDOUT ? 1 : -1;
}

void dab_cond_signal(dab_cond_t *cond) {
    (void)pthread_cond_signal(&cond->handle);
}

void dab_cond_broadcast(dab_cond_t *cond) {
    (void)pthread_cond_broadcast(&cond->handle);
}
#endif
