#ifndef DAB_OS_SHARED_LIB_H
#define DAB_OS_SHARED_LIB_H

#if defined(_WIN32)
#include <windows.h>
typedef struct dab_shared_lib {
    HMODULE handle;
} dab_shared_lib_t;
#else
typedef struct dab_shared_lib {
    void *handle;
} dab_shared_lib_t;
#endif

int dab_shared_lib_open(dab_shared_lib_t *lib, const char *path);
void *dab_shared_lib_symbol(dab_shared_lib_t *lib, const char *name);
void dab_shared_lib_close(dab_shared_lib_t *lib);

#endif
