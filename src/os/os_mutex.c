#include "dab/os/os_mutex.h"

#if defined(_WIN32)
#include <windows.h>

int dab_mutex_init(dab_mutex_t *mutex) {
    InitializeCriticalSection(&mutex->handle);
    return 0;
}

void dab_mutex_destroy(dab_mutex_t *mutex) {
    DeleteCriticalSection(&mutex->handle);
}

void dab_mutex_lock(dab_mutex_t *mutex) {
    EnterCriticalSection(&mutex->handle);
}

void dab_mutex_unlock(dab_mutex_t *mutex) {
    LeaveCriticalSection(&mutex->handle);
}
#else
#include <pthread.h>

int dab_mutex_init(dab_mutex_t *mutex) {
    return pthread_mutex_init(&mutex->handle, 0);
}

void dab_mutex_destroy(dab_mutex_t *mutex) {
    (void)pthread_mutex_destroy(&mutex->handle);
}

void dab_mutex_lock(dab_mutex_t *mutex) {
    (void)pthread_mutex_lock(&mutex->handle);
}

void dab_mutex_unlock(dab_mutex_t *mutex) {
    (void)pthread_mutex_unlock(&mutex->handle);
}
#endif
