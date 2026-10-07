#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE 1
#endif

#include "dab/os/os_thread.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <process.h>
#include <windows.h>

typedef struct {
    dab_thread_entry_t entry;
    void *arg;
} dab_thread_boot_t;

enum {
    DAB_WINDOWS_THREAD_STACK_BYTES = 8u * 1024u * 1024u
};

static unsigned __stdcall dab_thread_bootstrap(void *arg) {
    dab_thread_boot_t *boot = (dab_thread_boot_t *)arg;
    int result = boot->entry(boot->arg);
    free(boot);
    return (unsigned)result;
}

int dab_thread_create(dab_thread_t *thread, dab_thread_entry_t entry, void *arg, const char *name) {
    dab_thread_boot_t *boot = (dab_thread_boot_t *)malloc(sizeof(*boot));
    (void)name;
    if (boot == 0) {
        return -1;
    }
    boot->entry = entry;
    boot->arg = arg;
    thread->handle = (HANDLE)_beginthreadex(0,
                                            DAB_WINDOWS_THREAD_STACK_BYTES,
                                            dab_thread_bootstrap,
                                            boot,
                                            0,
                                            0);
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
    char name[64];
} dab_thread_boot_t;

enum {
    DAB_POSIX_THREAD_STACK_BYTES = 8u * 1024u * 1024u
};

static void *dab_thread_bootstrap(void *arg) {
    dab_thread_boot_t *boot = (dab_thread_boot_t *)arg;
#if defined(__APPLE__)
    if (boot->name[0] != '\0') {
        pthread_setname_np(boot->name);
    }
#elif defined(__linux__)
    if (boot->name[0] != '\0') {
        pthread_setname_np(pthread_self(), boot->name);
    }
#endif
    int result = boot->entry(boot->arg);
    free(boot);
    return (void *)(intptr_t)result;
}

int dab_thread_create(dab_thread_t *thread, dab_thread_entry_t entry, void *arg, const char *name) {
    pthread_attr_t attr;
    int attr_ready = 0;
    int create_rc;
    dab_thread_boot_t *boot = (dab_thread_boot_t *)malloc(sizeof(*boot));
    if (boot == 0) {
        return -1;
    }
    boot->entry = entry;
    boot->arg = arg;
    boot->name[0] = '\0';
    if (name != 0) {
        strncpy(boot->name, name, sizeof(boot->name) - 1u);
        boot->name[sizeof(boot->name) - 1u] = '\0';
    }

    if (pthread_attr_init(&attr) == 0) {
        attr_ready = 1;
        (void)pthread_attr_setstacksize(&attr, DAB_POSIX_THREAD_STACK_BYTES);
    }

    create_rc = pthread_create(&thread->handle,
                               attr_ready ? &attr : 0,
                               dab_thread_bootstrap,
                               boot);
    if (attr_ready) {
        pthread_attr_destroy(&attr);
    }
    if (create_rc != 0) {
        free(boot);
        return -1;
    }
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
