#include "dab/os/os_thread.h"

#include <stdlib.h>

#if defined(_WIN32)
#include <windows.h>

typedef struct {
    dab_thread_entry_t entry;
    void *arg;
} dab_thread_boot_t;

static DWORD WINAPI dab_thread_bootstrap(LPVOID arg) {
    dab_thread_boot_t *boot = (dab_thread_boot_t *)arg;
    int result = boot->entry(boot->arg);
    free(boot);
    return (DWORD)result;
}

int dab_thread_create(dab_thread_t *thread, dab_thread_entry_t entry, void *arg, const char *name) {
    dab_thread_boot_t *boot = (dab_thread_boot_t *)malloc(sizeof(*boot));
    (void)name;
    if (boot == 0) {
        return -1;
    }
    boot->entry = entry;
    boot->arg = arg;
    thread->handle = CreateThread(0, 0, dab_thread_bootstrap, boot, 0, 0);
    if (thread->handle == 0) {
        free(boot);
        return -1;
    }
    return 0;
}

int dab_thread_join(dab_thread_t *thread, int *result) {
    DWORD code = 0;
    if (WaitForSingleObject(thread->handle, INFINITE) != WAIT_OBJECT_0) {
        return -1;
    }
    if (!GetExitCodeThread(thread->handle, &code)) {
        CloseHandle(thread->handle);
        return -1;
    }
    CloseHandle(thread->handle);
    if (result != 0) {
        *result = (int)code;
    }
    return 0;
}
#else
#include <pthread.h>
#if defined(__APPLE__)
#include <pthread.h>
#endif

typedef struct {
    dab_thread_entry_t entry;
    void *arg;
} dab_thread_boot_t;

static void *dab_thread_bootstrap(void *arg) {
    dab_thread_boot_t *boot = (dab_thread_boot_t *)arg;
    int result = boot->entry(boot->arg);
    free(boot);
    return (void *)(intptr_t)result;
}

int dab_thread_create(dab_thread_t *thread, dab_thread_entry_t entry, void *arg, const char *name) {
    dab_thread_boot_t *boot = (dab_thread_boot_t *)malloc(sizeof(*boot));
    if (boot == 0) {
        return -1;
    }
    boot->entry = entry;
    boot->arg = arg;
    if (pthread_create(&thread->handle, 0, dab_thread_bootstrap, boot) != 0) {
        free(boot);
        return -1;
    }
#if defined(__APPLE__)
    if (name != 0) {
        pthread_setname_np(name);
    }
#elif defined(__linux__)
    if (name != 0) {
        pthread_setname_np(thread->handle, name);
    }
#else
    (void)name;
#endif
    return 0;
}

int dab_thread_join(dab_thread_t *thread, int *result) {
    void *value = 0;
    if (pthread_join(thread->handle, &value) != 0) {
        return -1;
    }
    if (result != 0) {
        *result = (int)(intptr_t)value;
    }
    return 0;
}
#endif
