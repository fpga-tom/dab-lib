#include "rtlsdr_api.h"

#include <string.h>

static const char *const DAB_RTLSDR_LIBRARY_NAMES[] = {
#if defined(_WIN32)
    "rtlsdr.dll",
    "librtlsdr.dll"
#elif defined(__APPLE__)
    "librtlsdr.dylib",
    "/opt/homebrew/lib/librtlsdr.dylib",
    "/usr/local/lib/librtlsdr.dylib"
#else
    "librtlsdr.so",
    "librtlsdr.so.0",
    "/usr/local/lib/librtlsdr.so"
#endif
};

int dab_rtlsdr_api_try_load(dab_rtlsdr_api_t *api) {
    unsigned int i;

    if (api == 0) {
        return -1;
    }
    memset(api, 0, sizeof(*api));

    for (i = 0u; i < (unsigned int)(sizeof(DAB_RTLSDR_LIBRARY_NAMES) / sizeof(DAB_RTLSDR_LIBRARY_NAMES[0])); ++i) {
        if (dab_shared_lib_open(&api->lib, DAB_RTLSDR_LIBRARY_NAMES[i]) == 0) {
            api->loaded = 1;
            break;
        }
    }
    if (!api->loaded) {
        return -1;
    }

    api->get_device_count = (dab_rtlsdr_get_device_count_fn)dab_shared_lib_symbol(&api->lib, "rtlsdr_get_device_count");
    api->get_device_name = (dab_rtlsdr_get_device_name_fn)dab_shared_lib_symbol(&api->lib, "rtlsdr_get_device_name");
    api->get_device_usb_strings = (dab_rtlsdr_get_device_usb_strings_fn)dab_shared_lib_symbol(&api->lib, "rtlsdr_get_device_usb_strings");
    api->open = (dab_rtlsdr_open_fn)dab_shared_lib_symbol(&api->lib, "rtlsdr_open");
    api->close = (dab_rtlsdr_close_fn)dab_shared_lib_symbol(&api->lib, "rtlsdr_close");
    api->set_center_freq = (dab_rtlsdr_set_center_freq_fn)dab_shared_lib_symbol(&api->lib, "rtlsdr_set_center_freq");
    api->set_sample_rate = (dab_rtlsdr_set_sample_rate_fn)dab_shared_lib_symbol(&api->lib, "rtlsdr_set_sample_rate");
    api->set_tuner_bandwidth = (dab_rtlsdr_set_tuner_bandwidth_fn)dab_shared_lib_symbol(&api->lib, "rtlsdr_set_tuner_bandwidth");
    api->set_tuner_gain_mode = (dab_rtlsdr_set_tuner_gain_mode_fn)dab_shared_lib_symbol(&api->lib, "rtlsdr_set_tuner_gain_mode");
    api->set_tuner_gain = (dab_rtlsdr_set_tuner_gain_fn)dab_shared_lib_symbol(&api->lib, "rtlsdr_set_tuner_gain");
    api->set_freq_correction = (dab_rtlsdr_set_freq_correction_fn)dab_shared_lib_symbol(&api->lib, "rtlsdr_set_freq_correction");
    api->set_bias_tee = (dab_rtlsdr_set_bias_tee_fn)dab_shared_lib_symbol(&api->lib, "rtlsdr_set_bias_tee");
    api->reset_buffer = (dab_rtlsdr_reset_buffer_fn)dab_shared_lib_symbol(&api->lib, "rtlsdr_reset_buffer");
    api->read_async = (dab_rtlsdr_read_async_fn)dab_shared_lib_symbol(&api->lib, "rtlsdr_read_async");
    api->cancel_async = (dab_rtlsdr_cancel_async_fn)dab_shared_lib_symbol(&api->lib, "rtlsdr_cancel_async");
    return 0;
}

int dab_rtlsdr_api_is_usable(const dab_rtlsdr_api_t *api) {
    if (api == 0 || !api->loaded) {
        return 0;
    }
    return api->get_device_count != 0 &&
           api->get_device_name != 0 &&
           api->get_device_usb_strings != 0 &&
           api->open != 0 &&
           api->close != 0 &&
           api->set_center_freq != 0 &&
           api->set_sample_rate != 0 &&
           api->set_tuner_gain_mode != 0 &&
           api->set_tuner_gain != 0 &&
           api->set_freq_correction != 0 &&
           api->reset_buffer != 0 &&
           api->read_async != 0 &&
           api->cancel_async != 0;
}

void dab_rtlsdr_api_unload(dab_rtlsdr_api_t *api) {
    if (api == 0) {
        return;
    }
    if (api->loaded) {
        dab_shared_lib_close(&api->lib);
    }
    memset(api, 0, sizeof(*api));
}
