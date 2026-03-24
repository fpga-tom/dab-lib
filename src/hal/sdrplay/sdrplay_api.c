#include "sdrplay_api.h"

#include <string.h>

static const char *const DAB_SDRPLAY_LIBRARY_NAMES[] = {
#if defined(_WIN32)
    "sdrplay_api.dll"
#elif defined(__APPLE__)
    "libsdrplay_api.dylib",
    "/usr/local/lib/libsdrplay_api.dylib",
    "/opt/homebrew/lib/libsdrplay_api.dylib"
#else
    "libsdrplay_api.so",
    "libsdrplay_api.so.3",
    "/usr/local/lib/libsdrplay_api.so"
#endif
};

int dab_sdrplay_api_try_load(dab_sdrplay_api_t *api) {
    unsigned int i;

    if (api == 0) {
        return -1;
    }
    memset(api, 0, sizeof(*api));

    for (i = 0u; i < (unsigned int)(sizeof(DAB_SDRPLAY_LIBRARY_NAMES) / sizeof(DAB_SDRPLAY_LIBRARY_NAMES[0])); ++i) {
        if (dab_shared_lib_open(&api->lib, DAB_SDRPLAY_LIBRARY_NAMES[i]) == 0) {
            api->loaded = 1;
            break;
        }
    }

    if (!api->loaded) {
        return -1;
    }

    api->has_open_api = dab_shared_lib_symbol(&api->lib, "sdrplay_api_Open") != 0;
    api->has_close_api = dab_shared_lib_symbol(&api->lib, "sdrplay_api_Close") != 0;
    api->has_api_version = dab_shared_lib_symbol(&api->lib, "sdrplay_api_ApiVersion") != 0;
    return 0;
}

void dab_sdrplay_api_unload(dab_sdrplay_api_t *api) {
    if (api == 0) {
        return;
    }
    if (api->loaded) {
        dab_shared_lib_close(&api->lib);
    }
    memset(api, 0, sizeof(*api));
}
