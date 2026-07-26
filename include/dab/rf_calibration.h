#ifndef DAB_RF_CALIBRATION_H
#define DAB_RF_CALIBRATION_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DAB_RF_CALIBRATION_PROFILE_VERSION 1u
#define DAB_RF_CALIBRATION_MAX_POINTS 64u
#define DAB_RF_CALIBRATION_ID_CAPACITY 64u
#define DAB_RF_CALIBRATION_NAME_CAPACITY 96u
#define DAB_RF_CALIBRATION_DRIVER_CAPACITY 16u
#define DAB_RF_CALIBRATION_SERIAL_CAPACITY 64u

typedef enum {
    DAB_RF_CALIBRATION_VALID = 0,
    DAB_RF_CALIBRATION_NO_PROFILE = 1,
    DAB_RF_CALIBRATION_AGC_ACTIVE = 2,
    DAB_RF_CALIBRATION_TELEMETRY_UNAVAILABLE = 3,
    DAB_RF_CALIBRATION_DEVICE_MISMATCH = 4,
    DAB_RF_CALIBRATION_SETTINGS_MISMATCH = 5,
    DAB_RF_CALIBRATION_GAIN_MISMATCH = 6,
    DAB_RF_CALIBRATION_OUTSIDE_FREQUENCY_RANGE = 7,
    DAB_RF_CALIBRATION_INVALID_PROFILE = 8
} dab_rf_calibration_status_t;

typedef struct {
    uint64_t frequency_hz;
    uint8_t lna_state;
    int gain_reduction_db;
    float reference_power_dbm;
    float measured_power_dbfs;
    float correction_db;
    float uncertainty_db;
} dab_rf_calibration_point_t;

typedef struct {
    uint32_t version;
    char id[DAB_RF_CALIBRATION_ID_CAPACITY];
    char name[DAB_RF_CALIBRATION_NAME_CAPACITY];
    char driver[DAB_RF_CALIBRATION_DRIVER_CAPACITY];
    char device_serial[DAB_RF_CALIBRATION_SERIAL_CAPACITY];
    uint8_t hardware_version;
    uint32_t sample_rate_hz;
    uint32_t bandwidth_hz;
    uint8_t if_mode;
    uint8_t antenna;
    uint32_t frequency_tolerance_hz;
    float default_uncertainty_db;
    uint32_t point_count;
    dab_rf_calibration_point_t points[DAB_RF_CALIBRATION_MAX_POINTS];
} dab_rf_calibration_profile_t;

typedef struct {
    const char *driver;
    const char *device_serial;
    uint8_t hardware_version;
    uint64_t frequency_hz;
    uint32_t sample_rate_hz;
    uint32_t bandwidth_hz;
    uint8_t if_mode;
    uint8_t antenna;
    int agc_enabled;
    int applied_gain_valid;
    uint8_t lna_state;
    int gain_reduction_db;
} dab_rf_calibration_context_t;

typedef struct {
    dab_rf_calibration_status_t status;
    int valid;
    float calibrated_power_dbm;
    float correction_db;
    float uncertainty_db;
    uint64_t lower_frequency_hz;
    uint64_t upper_frequency_hz;
    char profile_id[DAB_RF_CALIBRATION_ID_CAPACITY];
} dab_rf_calibration_result_t;

void dab_rf_calibration_profile_init(dab_rf_calibration_profile_t *profile);
int dab_rf_calibration_profile_add_point(
    dab_rf_calibration_profile_t *profile,
    const dab_rf_calibration_point_t *point);
dab_rf_calibration_status_t dab_rf_calibration_apply(
    const dab_rf_calibration_profile_t *profile,
    const dab_rf_calibration_context_t *context,
    float measured_power_dbfs,
    dab_rf_calibration_result_t *result);
const char *dab_rf_calibration_status_text(dab_rf_calibration_status_t status);

#ifdef __cplusplus
}
#endif

#endif
