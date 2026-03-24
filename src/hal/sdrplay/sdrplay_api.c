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

    api->open_api = (dab_sdrplay_api_open_fn)dab_shared_lib_symbol(&api->lib, "sdrplay_api_Open");
    api->close_api = (dab_sdrplay_api_close_fn)dab_shared_lib_symbol(&api->lib, "sdrplay_api_Close");
    api->api_version = (dab_sdrplay_api_api_version_fn)dab_shared_lib_symbol(&api->lib, "sdrplay_api_ApiVersion");
    api->get_devices = (dab_sdrplay_api_get_devices_fn)dab_shared_lib_symbol(&api->lib, "sdrplay_api_GetDevices");
    api->select_device = (dab_sdrplay_api_select_device_fn)dab_shared_lib_symbol(&api->lib, "sdrplay_api_SelectDevice");
    api->release_device = (dab_sdrplay_api_release_device_fn)dab_shared_lib_symbol(&api->lib, "sdrplay_api_ReleaseDevice");
    api->init = (dab_sdrplay_api_init_fn)dab_shared_lib_symbol(&api->lib, "sdrplay_api_Init");
    api->uninit = (dab_sdrplay_api_uninit_fn)dab_shared_lib_symbol(&api->lib, "sdrplay_api_Uninit");
    return 0;
}

int dab_sdrplay_api_is_usable(const dab_sdrplay_api_t *api) {
    if (api == 0 || !api->loaded) {
        return 0;
    }
    return api->open_api != 0 &&
           api->close_api != 0 &&
           api->api_version != 0 &&
           api->get_devices != 0 &&
           api->select_device != 0 &&
           api->release_device != 0 &&
           api->init != 0 &&
           api->uninit != 0;
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
