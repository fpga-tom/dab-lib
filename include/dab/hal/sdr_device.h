#ifndef DAB_HAL_SDR_DEVICE_H
#define DAB_HAL_SDR_DEVICE_H

#include <stdint.h>

#include "dab/config.h"

struct dab_sdr_device;
struct dab_sdr_stream;

typedef enum {
    DAB_SDR_STATUS_OK = 0,
    DAB_SDR_STATUS_ERROR = -1,
    DAB_SDR_STATUS_INVALID_ARGUMENT = -2,
    DAB_SDR_STATUS_UNSUPPORTED = -3,
    DAB_SDR_STATUS_NOT_FOUND = -4,
    DAB_SDR_STATUS_NOT_IMPLEMENTED = -5,
    DAB_SDR_STATUS_BUSY = -6,
    DAB_SDR_STATUS_TIMEOUT = -7
} dab_sdr_status_t;

typedef enum {
    DAB_SDR_DRIVER_UNKNOWN = 0,
    DAB_SDR_DRIVER_SDRPLAY = 1,
    DAB_SDR_DRIVER_RTLSDR = 2
} dab_sdr_driver_kind_t;

typedef struct {
    uint32_t min_sample_rate;
    uint32_t max_sample_rate;
    uint64_t min_center_freq_hz;
    uint64_t max_center_freq_hz;
    int min_gain_db;
    int max_gain_db;
} dab_sdr_capabilities_t;

typedef struct {
    const char *id;
    const char *driver;
    const char *serial;
    const char *label;
    dab_sdr_driver_kind_t kind;
    dab_sdr_capabilities_t caps;
} dab_sdr_device_desc_t;

typedef struct {
    int16_t *iq_interleaved;
    uint32_t sample_count;
    uint64_t sample_index;
    uint64_t timestamp_ns;
} dab_sdr_block_view_t;

typedef struct {
    dab_sdr_status_t (*start)(struct dab_sdr_stream *stream);
    dab_sdr_status_t (*stop)(struct dab_sdr_stream *stream);
    dab_sdr_status_t (*read)(struct dab_sdr_stream *stream, dab_sdr_block_view_t *block);
    dab_sdr_status_t (*read_timeout)(struct dab_sdr_stream *stream,
                                     dab_sdr_block_view_t *block,
                                     uint32_t timeout_ms);
    void (*destroy)(struct dab_sdr_stream *stream);
} dab_sdr_stream_vtbl_t;

typedef struct dab_sdr_stream {
    const dab_sdr_stream_vtbl_t *vtbl;
} dab_sdr_stream_t;

typedef struct {
    dab_sdr_status_t (*configure)(struct dab_sdr_device *device, const dab_stream_config_t *config);
    dab_sdr_status_t (*open_stream)(struct dab_sdr_device *device, dab_sdr_stream_t **stream);
    void (*destroy)(struct dab_sdr_device *device);
} dab_sdr_device_vtbl_t;

typedef struct dab_sdr_device {
    const dab_sdr_device_vtbl_t *vtbl;
    dab_sdr_device_desc_t desc;
} dab_sdr_device_t;

typedef struct {
    const char *driver;
    const char *device_id;
    const char *serial;
} dab_sdr_open_params_t;

uint32_t dab_sdr_device_count(void);
const dab_sdr_device_desc_t *dab_sdr_device_desc(uint32_t index);
dab_sdr_status_t dab_sdr_device_open(const dab_sdr_open_params_t *params, dab_sdr_device_t **device);
void dab_sdr_device_close(dab_sdr_device_t *device);
void dab_sdr_stream_close(dab_sdr_stream_t *stream);

#endif
