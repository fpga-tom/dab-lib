#include "dab/hal/sdr_device.h"
#include "dab/hal/sdrplay_device.h"

#include "test_support.h"

void test_sdrplay_device_specific_config_propagates_to_samples(void) {
    dab_sdr_open_params_t params = {"sdrplay", NULL, NULL};
    dab_sdr_device_t *device = 0;
    dab_sdr_stream_t *stream = 0;
    dab_stream_config_t stream_config;
    dab_sdrplay_config_t device_config;
    dab_sdr_block_view_t block;

    TEST_ASSERT(dab_sdr_device_open(&params, &device) == DAB_SDR_STATUS_OK);

    dab_sdrplay_default_config(&device_config);
    device_config.gain_mode = DAB_SDRPLAY_GAIN_MODE_MANUAL;
    device_config.if_mode = DAB_SDRPLAY_IF_MODE_LOW_IF;
    device_config.antenna = DAB_SDRPLAY_ANTENNA_B;
    device_config.lna_state = 2u;
    device_config.if_gain_db = 5;
    device_config.bias_tee_enabled = 1;
    device_config.ppm_correction = 12;

    TEST_ASSERT(dab_sdrplay_configure_device(device, &device_config) == DAB_SDR_STATUS_OK);

    stream_config.sample_rate = 2048000u;
    stream_config.center_freq_hz = 220352000ull;
    stream_config.bandwidth_hz = 1536000u;
    stream_config.gain_db = 30;

    TEST_ASSERT(device->vtbl->configure(device, &stream_config) == DAB_SDR_STATUS_OK);
    TEST_ASSERT(device->vtbl->open_stream(device, &stream) == DAB_SDR_STATUS_OK);
    TEST_ASSERT(stream->vtbl->start(stream) == DAB_SDR_STATUS_OK);
    TEST_ASSERT(stream->vtbl->read(stream, &block) == DAB_SDR_STATUS_OK);

    TEST_ASSERT(block.iq_interleaved[0] == 56);
    TEST_ASSERT(block.iq_interleaved[1] == 0);

    TEST_ASSERT(stream->vtbl->stop(stream) == DAB_SDR_STATUS_OK);
    dab_sdr_stream_close(stream);
    dab_sdr_device_close(device);
}

void test_sdrplay_rejects_invalid_device_specific_config(void) {
    dab_sdr_open_params_t params = {"sdrplay", NULL, NULL};
    dab_sdr_device_t *device = 0;
    dab_sdrplay_config_t config;

    TEST_ASSERT(dab_sdr_device_open(&params, &device) == DAB_SDR_STATUS_OK);

    dab_sdrplay_default_config(&config);
    config.lna_state = 42u;

    TEST_ASSERT(dab_sdrplay_configure_device(device, &config) == DAB_SDR_STATUS_UNSUPPORTED);

    dab_sdr_device_close(device);
}
