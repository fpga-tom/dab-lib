#ifndef DAB_HAL_SDRPLAY_API_H
#define DAB_HAL_SDRPLAY_API_H

#include "dab/os/os_shared_lib.h"
#include "sdrplay_api_types.h"

typedef int (*dab_sdrplay_api_open_fn)(void);
typedef int (*dab_sdrplay_api_close_fn)(void);
typedef int (*dab_sdrplay_api_api_version_fn)(float *api_version);
typedef int (*dab_sdrplay_api_get_devices_fn)(dab_sdrplay_api_device_t *devices,
                                              unsigned int *num_devices,
                                              unsigned int max_devices);
typedef int (*dab_sdrplay_api_select_device_fn)(dab_sdrplay_api_device_t *device);
typedef int (*dab_sdrplay_api_release_device_fn)(dab_sdrplay_api_device_t *device);
typedef int (*dab_sdrplay_api_get_device_params_fn)(void *dev,
                                                    dab_sdrplay_device_params_t **device_params);
typedef int (*dab_sdrplay_api_init_fn)(void *dev,
                                       dab_sdrplay_api_callbacks_t *callbacks,
                                       void *context);
typedef int (*dab_sdrplay_api_uninit_fn)(void *dev);
typedef int (*dab_sdrplay_api_update_fn)(void *dev,
                                         dab_sdrplay_tuner_select_t tuner,
                                         dab_sdrplay_reason_for_update_t reason_for_update,
                                         dab_sdrplay_reason_for_update_ext1_t reason_for_update_ext1);
typedef const char *(*dab_sdrplay_api_get_error_string_fn)(int err);

typedef struct {
    dab_shared_lib_t lib;
    int loaded;
    dab_sdrplay_api_open_fn open_api;
    dab_sdrplay_api_close_fn close_api;
    dab_sdrplay_api_api_version_fn api_version;
    dab_sdrplay_api_get_devices_fn get_devices;
    dab_sdrplay_api_select_device_fn select_device;
    dab_sdrplay_api_release_device_fn release_device;
    dab_sdrplay_api_get_device_params_fn get_device_params;
    dab_sdrplay_api_init_fn init;
    dab_sdrplay_api_uninit_fn uninit;
    dab_sdrplay_api_update_fn update;
    dab_sdrplay_api_get_error_string_fn get_error_string;
} dab_sdrplay_api_t;

int dab_sdrplay_api_try_load(dab_sdrplay_api_t *api);
int dab_sdrplay_api_is_usable(const dab_sdrplay_api_t *api);
void dab_sdrplay_api_unload(dab_sdrplay_api_t *api);

#endif
