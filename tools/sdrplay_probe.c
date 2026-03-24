#include <stdio.h>
#include <stdlib.h>

#include "dab/hal/sdr_device.h"
#include "dab/hal/sdrplay_device.h"

int main(void) {
    dab_sdr_open_params_t params = {"sdrplay", NULL, NULL};
    dab_sdr_device_t *device = 0;
    dab_sdr_stream_t *stream = 0;
    dab_stream_config_t stream_config;
    dab_sdrplay_config_t device_config;
    dab_sdr_block_view_t block;
    int rc = 1;

    fprintf(stderr, "open device\n");
    fflush(stderr);
    if (dab_sdr_device_open(&params, &device) != DAB_SDR_STATUS_OK || device == 0) {
        fprintf(stderr, "open failed\n");
        return 2;
    }

    dab_sdrplay_default_config(&device_config);
    device_config.gain_mode = DAB_SDRPLAY_GAIN_MODE_AUTO;
    device_config.if_mode = DAB_SDRPLAY_IF_MODE_ZERO_IF;
    device_config.antenna = DAB_SDRPLAY_ANTENNA_A;

    stream_config.sample_rate = 2048000u;
    stream_config.center_freq_hz = 220352000ull;
    stream_config.bandwidth_hz = 1536000u;
    stream_config.gain_db = 40;

    fprintf(stderr, "configure device-specific\n");
    fflush(stderr);
    if (dab_sdrplay_configure_device(device, &device_config) != DAB_SDR_STATUS_OK) {
        fprintf(stderr, "device config failed\n");
        goto cleanup;
    }

    fprintf(stderr, "configure stream\n");
    fflush(stderr);
    if (device->vtbl->configure(device, &stream_config) != DAB_SDR_STATUS_OK) {
        fprintf(stderr, "stream config failed\n");
        goto cleanup;
    }

    fprintf(stderr, "open stream\n");
    fflush(stderr);
    if (device->vtbl->open_stream(device, &stream) != DAB_SDR_STATUS_OK || stream == 0) {
        fprintf(stderr, "open stream failed\n");
        goto cleanup;
    }

    fprintf(stderr, "start stream\n");
    fflush(stderr);
    if (stream->vtbl->start(stream) != DAB_SDR_STATUS_OK) {
        fprintf(stderr, "start failed\n");
        goto cleanup;
    }

    fprintf(stderr, "read block\n");
    fflush(stderr);
    if (stream->vtbl->read(stream, &block) != DAB_SDR_STATUS_OK) {
        fprintf(stderr, "read failed\n");
        goto cleanup;
    }

    printf("samples=%u first_i=%d first_q=%d sample_index=%llu timestamp_ns=%llu\n",
           block.sample_count,
           block.iq_interleaved[0],
           block.iq_interleaved[1],
           (unsigned long long)block.sample_index,
           (unsigned long long)block.timestamp_ns);
    rc = 0;

cleanup:
    if (stream != 0) {
        (void)stream->vtbl->stop(stream);
        dab_sdr_stream_close(stream);
    }
    if (device != 0) {
        dab_sdr_device_close(device);
    }
    return rc;
}
