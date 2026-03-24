#include "dab/os/os_shared_lib.h"

#if defined(_WIN32)
#include <windows.h>

int dab_shared_lib_open(dab_shared_lib_t *lib, const char *path) {
    if (lib == 0 || path == 0) {
        return -1;
    }
    lib->handle = LoadLibraryA(path);
    return lib->handle == 0 ? -1 : 0;
}

void *dab_shared_lib_symbol(dab_shared_lib_t *lib, const char *name) {
    if (lib == 0 || lib->handle == 0 || name == 0) {
        return 0;
    }
    return (void *)GetProcAddress(lib->handle, name);
}

void dab_shared_lib_close(dab_shared_lib_t *lib) {
    if (lib != 0 && lib->handle != 0) {
        FreeLibrary(lib->handle);
        lib->handle = 0;
    }
}
#else
#include <dlfcn.h>

int dab_shared_lib_open(dab_shared_lib_t *lib, const char *path) {
    if (lib == 0 || path == 0) {
        return -1;
    }
    lib->handle = dlopen(path, RTLD_LAZY | RTLD_LOCAL);
    return lib->handle == 0 ? -1 : 0;
}

void *dab_shared_lib_symbol(dab_shared_lib_t *lib, const char *name) {
    if (lib == 0 || lib->handle == 0 || name == 0) {
        return 0;
    }
    return dlsym(lib->handle, name);
}

void dab_shared_lib_close(dab_shared_lib_t *lib) {
    if (lib != 0 && lib->handle != 0) {
        dlclose(lib->handle);
        lib->handle = 0;
    }
}
#endif
