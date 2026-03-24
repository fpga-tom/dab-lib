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

void dab_cond_signal(dab_cond_t *cond) {
    WakeConditionVariable(&cond->handle);
}

void dab_cond_broadcast(dab_cond_t *cond) {
    WakeAllConditionVariable(&cond->handle);
}
#else
#include <pthread.h>

int dab_cond_init(dab_cond_t *cond) {
    return pthread_cond_init(&cond->handle, 0);
}

void dab_cond_destroy(dab_cond_t *cond) {
    (void)pthread_cond_destroy(&cond->handle);
}

void dab_cond_wait(dab_cond_t *cond, dab_mutex_t *mutex) {
    (void)pthread_cond_wait(&cond->handle, &mutex->handle);
}

void dab_cond_signal(dab_cond_t *cond) {
    (void)pthread_cond_signal(&cond->handle);
}

void dab_cond_broadcast(dab_cond_t *cond) {
    (void)pthread_cond_broadcast(&cond->handle);
}
#endif
