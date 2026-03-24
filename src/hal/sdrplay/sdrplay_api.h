#ifndef DAB_HAL_SDRPLAY_API_H
#define DAB_HAL_SDRPLAY_API_H

#include "dab/os/os_shared_lib.h"

typedef struct {
    dab_shared_lib_t lib;
    int loaded;
    int has_open_api;
    int has_close_api;
    int has_api_version;
} dab_sdrplay_api_t;

int dab_sdrplay_api_try_load(dab_sdrplay_api_t *api);
void dab_sdrplay_api_unload(dab_sdrplay_api_t *api);

#endif
