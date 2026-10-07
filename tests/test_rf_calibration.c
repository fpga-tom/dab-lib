#include "dab/rf_calibration.h"

#include <string.h>

#include "test_support.h"

static dab_rf_calibration_profile_t calibration_profile(void) {
    dab_rf_calibration_profile_t profile;
    dab_rf_calibration_point_t point;

    dab_rf_calibration_profile_init(&profile);
    memcpy(profile.id, "test-band3", sizeof("test-band3"));
    memcpy(profile.driver, "sdrplay", sizeof("sdrplay"));
    memcpy(profile.device_serial, "B000123", sizeof("B000123"));
    profile.hardware_version = 1u;
    profile.sample_rate_hz = 2048000u;
    profile.bandwidth_hz = 1536000u;
    profile.if_mode = 0u;
    profile.antenna = 1u;
    profile.default_uncertainty_db = 0.8f;

    memset(&point, 0, sizeof(point));
    point.frequency_hz = 211648000ull;
    point.lna_state = 3u;
    point.gain_reduction_db = 32;
    point.reference_power_dbm = -60.0f;
    point.measured_power_dbfs = -21.0f;
    TEST_ASSERT(dab_rf_calibration_profile_add_point(&profile, &point) == 0);

    point.frequency_hz = 215072000ull;
    point.reference_power_dbm = -60.0f;
    point.measured_power_dbfs = -23.0f;
    TEST_ASSERT(dab_rf_calibration_profile_add_point(&profile, &point) == 0);
    return profile;
}

static dab_rf_calibration_context_t calibration_context(void) {
    dab_rf_calibration_context_t context;
    memset(&context, 0, sizeof(context));
    context.driver = "sdrplay";
    context.device_serial = "B000123";
    context.hardware_version = 1u;
    context.frequency_hz = 213360000ull;
    context.sample_rate_hz = 2048000u;
    context.bandwidth_hz = 1536000u;
    context.if_mode = 0u;
    context.antenna = 1u;
    context.applied_gain_valid = 1;
    context.lna_state = 3u;
    context.gain_reduction_db = 32;
    return context;
}

void test_rf_calibration_interpolates_matching_operating_point(void) {
    dab_rf_calibration_profile_t profile = calibration_profile();
    dab_rf_calibration_context_t context = calibration_context();
    dab_rf_calibration_result_t result;

    TEST_ASSERT(dab_rf_calibration_apply(&profile,
                                         &context,
                                         -22.0f,
                                         &result) == DAB_RF_CALIBRATION_VALID);
    TEST_ASSERT(result.valid == 1);
    TEST_ASSERT(result.calibrated_power_dbm > -60.01f);
    TEST_ASSERT(result.calibrated_power_dbm < -59.99f);
    TEST_ASSERT(result.uncertainty_db > 0.79f);
    TEST_ASSERT(result.uncertainty_db < 0.81f);
}

void test_rf_calibration_rejects_agc_and_gain_mismatch(void) {
    dab_rf_calibration_profile_t profile = calibration_profile();
    dab_rf_calibration_context_t context = calibration_context();
    dab_rf_calibration_result_t result;

    context.agc_enabled = 1;
    TEST_ASSERT(dab_rf_calibration_apply(&profile,
                                         &context,
                                         -22.0f,
                                         &result) == DAB_RF_CALIBRATION_AGC_ACTIVE);
    TEST_ASSERT(result.valid == 0);

    context.agc_enabled = 0;
    context.gain_reduction_db = 33;
    TEST_ASSERT(dab_rf_calibration_apply(&profile,
                                         &context,
                                         -22.0f,
                                         &result) == DAB_RF_CALIBRATION_GAIN_MISMATCH);
    TEST_ASSERT(result.valid == 0);
}

void test_rf_calibration_replaces_duplicate_operating_point(void) {
    dab_rf_calibration_profile_t profile = calibration_profile();
    dab_rf_calibration_point_t replacement = profile.points[0];

    replacement.reference_power_dbm = -55.0f;
    TEST_ASSERT(dab_rf_calibration_profile_add_point(&profile, &replacement) == 0);
    TEST_ASSERT(profile.point_count == 2u);
    TEST_ASSERT(profile.points[0].correction_db > -34.01f);
    TEST_ASSERT(profile.points[0].correction_db < -33.99f);
}
