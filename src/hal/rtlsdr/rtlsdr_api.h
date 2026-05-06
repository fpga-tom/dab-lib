#ifndef DAB_HAL_RTLSDR_API_H
#define DAB_HAL_RTLSDR_API_H

#include <stdint.h>

#include "dab/os/os_shared_lib.h"

typedef struct rtlsdr_dev dab_rtlsdr_dev_t;

typedef void (*dab_rtlsdr_read_async_cb_t)(unsigned char *buf, uint32_t len, void *ctx);

typedef uint32_t (*dab_rtlsdr_get_device_count_fn)(void);
typedef const char *(*dab_rtlsdr_get_device_name_fn)(uint32_t index);
typedef int (*dab_rtlsdr_get_device_usb_strings_fn)(uint32_t index, char *manufact, char *product, char *serial);
typedef int (*dab_rtlsdr_open_fn)(dab_rtlsdr_dev_t **dev, uint32_t index);
typedef int (*dab_rtlsdr_close_fn)(dab_rtlsdr_dev_t *dev);
typedef int (*dab_rtlsdr_set_center_freq_fn)(dab_rtlsdr_dev_t *dev, uint32_t freq);
typedef int (*dab_rtlsdr_set_sample_rate_fn)(dab_rtlsdr_dev_t *dev, uint32_t rate);
typedef int (*dab_rtlsdr_set_tuner_bandwidth_fn)(dab_rtlsdr_dev_t *dev, uint32_t bw);
typedef int (*dab_rtlsdr_set_tuner_gain_mode_fn)(dab_rtlsdr_dev_t *dev, int mode);
typedef int (*dab_rtlsdr_set_tuner_gain_fn)(dab_rtlsdr_dev_t *dev, int gain);
typedef int (*dab_rtlsdr_get_tuner_gains_fn)(dab_rtlsdr_dev_t *dev, int *gains);
typedef int (*dab_rtlsdr_set_freq_correction_fn)(dab_rtlsdr_dev_t *dev, int ppm);
typedef int (*dab_rtlsdr_set_bias_tee_fn)(dab_rtlsdr_dev_t *dev, int on);
typedef int (*dab_rtlsdr_reset_buffer_fn)(dab_rtlsdr_dev_t *dev);
typedef int (*dab_rtlsdr_read_async_fn)(dab_rtlsdr_dev_t *dev,
                                        dab_rtlsdr_read_async_cb_t cb,
                                        void *ctx,
                                        uint32_t buf_num,
                                        uint32_t buf_len);
typedef int (*dab_rtlsdr_cancel_async_fn)(dab_rtlsdr_dev_t *dev);

typedef struct {
    dab_shared_lib_t lib;
    int loaded;
    dab_rtlsdr_get_device_count_fn get_device_count;
    dab_rtlsdr_get_device_name_fn get_device_name;
    dab_rtlsdr_get_device_usb_strings_fn get_device_usb_strings;
    dab_rtlsdr_open_fn open;
    dab_rtlsdr_close_fn close;
    dab_rtlsdr_set_center_freq_fn set_center_freq;
    dab_rtlsdr_set_sample_rate_fn set_sample_rate;
    dab_rtlsdr_set_tuner_bandwidth_fn set_tuner_bandwidth;
    dab_rtlsdr_set_tuner_gain_mode_fn set_tuner_gain_mode;
    dab_rtlsdr_set_tuner_gain_fn set_tuner_gain;
    dab_rtlsdr_get_tuner_gains_fn get_tuner_gains;
    dab_rtlsdr_set_freq_correction_fn set_freq_correction;
    dab_rtlsdr_set_bias_tee_fn set_bias_tee;
    dab_rtlsdr_reset_buffer_fn reset_buffer;
    dab_rtlsdr_read_async_fn read_async;
    dab_rtlsdr_cancel_async_fn cancel_async;
} dab_rtlsdr_api_t;

int dab_rtlsdr_api_try_load(dab_rtlsdr_api_t *api);
int dab_rtlsdr_api_is_usable(const dab_rtlsdr_api_t *api);
void dab_rtlsdr_api_unload(dab_rtlsdr_api_t *api);

#endif
