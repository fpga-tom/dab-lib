#include "dab/hal/sdr_device.h"

#include "test_support.h"

void test_hal_enumerates_sdrplay_stub(void) {
    uint32_t count = dab_sdr_device_count();
    const dab_sdr_device_desc_t *desc = dab_sdr_device_desc(0u);

    TEST_ASSERT(count >= 1u);
    TEST_ASSERT(desc != 0);
    TEST_ASSERT(desc->kind == DAB_SDR_DRIVER_SDRPLAY);
    TEST_ASSERT(desc->driver != 0);
}

void test_hal_opens_and_configures_sdrplay_stub(void) {
    dab_sdr_open_params_t params = {"sdrplay", NULL, NULL};
    dab_sdr_device_t *device = 0;
    dab_sdr_stream_t *stream = 0;
    dab_stream_config_t config;
    dab_sdr_block_view_t block0;
    dab_sdr_block_view_t block1;

    TEST_ASSERT(dab_sdr_device_open(&params, &device) == DAB_SDR_STATUS_OK);
    TEST_ASSERT(device != 0);
    TEST_ASSERT(device->desc.kind == DAB_SDR_DRIVER_SDRPLAY);

    config.sample_rate = 2048000u;
    config.center_freq_hz = 220352000ull;
    config.bandwidth_hz = 1536000u;
    config.gain_db = 30;

    TEST_ASSERT(device->vtbl->configure(device, &config) == DAB_SDR_STATUS_OK);
    TEST_ASSERT(device->vtbl->open_stream(device, &stream) == DAB_SDR_STATUS_OK);
    TEST_ASSERT(stream != 0);
    TEST_ASSERT(stream->vtbl->start(stream) == DAB_SDR_STATUS_OK);
    TEST_ASSERT(stream->vtbl->read(stream, &block0) == DAB_SDR_STATUS_OK);
    TEST_ASSERT(block0.sample_count == 1024u);
    TEST_ASSERT(block0.iq_interleaved != 0);
    TEST_ASSERT(block0.iq_interleaved[0] == 0);
    TEST_ASSERT(block0.iq_interleaved[1] == 0);
    TEST_ASSERT(stream->vtbl->read(stream, &block1) == DAB_SDR_STATUS_OK);
    TEST_ASSERT(block1.sample_count == 1024u);
    TEST_ASSERT(block1.sample_index >= block0.sample_index + block0.sample_count);
    TEST_ASSERT(block1.timestamp_ns >= block0.timestamp_ns);
    TEST_ASSERT(stream->vtbl->stop(stream) == DAB_SDR_STATUS_OK);

    dab_sdr_stream_close(stream);
    dab_sdr_device_close(device);
}

void test_hal_rejects_unknown_driver(void) {
    dab_sdr_open_params_t params = {"unknown", NULL, NULL};
    dab_sdr_device_t *device = 0;

    TEST_ASSERT(dab_sdr_device_open(&params, &device) == DAB_SDR_STATUS_NOT_FOUND);
    TEST_ASSERT(device == 0);
}
