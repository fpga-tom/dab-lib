#include "dab/rf_calibration.h"

#include <math.h>
#include <stddef.h>
#include <string.h>

static int dab_rf_calibration_string_matches(const char *expected,
                                             const char *actual) {
    if (expected == 0 || expected[0] == '\0') {
        return 1;
    }
    return actual != 0 && strcmp(expected, actual) == 0;
}

static uint64_t dab_rf_calibration_frequency_delta(uint64_t left,
                                                   uint64_t right) {
    return left >= right ? left - right : right - left;
}

static void dab_rf_calibration_copy_id(char *destination,
                                       size_t capacity,
                                       const char *source) {
    size_t length;
    if (destination == 0 || capacity == 0u) {
        return;
    }
    destination[0] = '\0';
    if (source == 0) {
        return;
    }
    length = strlen(source);
    if (length >= capacity) {
        length = capacity - 1u;
    }
    memcpy(destination, source, length);
    destination[length] = '\0';
}

void dab_rf_calibration_profile_init(dab_rf_calibration_profile_t *profile) {
    if (profile == 0) {
        return;
    }
    memset(profile, 0, sizeof(*profile));
    profile->version = DAB_RF_CALIBRATION_PROFILE_VERSION;
    profile->frequency_tolerance_hz = 1000u;
    profile->default_uncertainty_db = 1.0f;
}

int dab_rf_calibration_profile_add_point(
    dab_rf_calibration_profile_t *profile,
    const dab_rf_calibration_point_t *point) {
    dab_rf_calibration_point_t stored;
    uint32_t index;

    if (profile == 0 || point == 0 || point->frequency_hz == 0u ||
        !isfinite(point->reference_power_dbm) ||
        !isfinite(point->measured_power_dbfs)) {
        return -1;
    }

    stored = *point;
    stored.correction_db =
        stored.reference_power_dbm - stored.measured_power_dbfs;
    if (!isfinite(stored.uncertainty_db) || stored.uncertainty_db <= 0.0f) {
        stored.uncertainty_db =
            profile->default_uncertainty_db > 0.0f
                ? profile->default_uncertainty_db
                : 1.0f;
    }

    for (index = 0u; index < profile->point_count; ++index) {
        dab_rf_calibration_point_t *existing = &profile->points[index];
        if (existing->frequency_hz == stored.frequency_hz &&
            existing->lna_state == stored.lna_state &&
            existing->gain_reduction_db == stored.gain_reduction_db) {
            *existing = stored;
            return 0;
        }
    }
    if (profile->point_count >= DAB_RF_CALIBRATION_MAX_POINTS) {
        return -1;
    }
    profile->points[profile->point_count++] = stored;
    return 0;
}

dab_rf_calibration_status_t dab_rf_calibration_apply(
    const dab_rf_calibration_profile_t *profile,
    const dab_rf_calibration_context_t *context,
    float measured_power_dbfs,
    dab_rf_calibration_result_t *result) {
    const dab_rf_calibration_point_t *exact = 0;
    const dab_rf_calibration_point_t *lower = 0;
    const dab_rf_calibration_point_t *upper = 0;
    uint64_t exact_delta = UINT64_MAX;
    uint32_t matching_gain_points = 0u;
    uint32_t index;
    float correction;
    float uncertainty;

    if (result == 0) {
        return DAB_RF_CALIBRATION_INVALID_PROFILE;
    }
    memset(result, 0, sizeof(*result));
    result->status = DAB_RF_CALIBRATION_INVALID_PROFILE;

    if (profile == 0 || profile->point_count == 0u) {
        result->status = DAB_RF_CALIBRATION_NO_PROFILE;
        return result->status;
    }
    dab_rf_calibration_copy_id(result->profile_id,
                               sizeof(result->profile_id),
                               profile->id);
    if (context == 0 || !isfinite(measured_power_dbfs) ||
        profile->version != DAB_RF_CALIBRATION_PROFILE_VERSION ||
        profile->point_count > DAB_RF_CALIBRATION_MAX_POINTS) {
        return result->status;
    }
    if (context->agc_enabled) {
        result->status = DAB_RF_CALIBRATION_AGC_ACTIVE;
        return result->status;
    }
    if (!context->applied_gain_valid) {
        result->status = DAB_RF_CALIBRATION_TELEMETRY_UNAVAILABLE;
        return result->status;
    }
    if (!dab_rf_calibration_string_matches(profile->driver,
                                           context->driver) ||
        !dab_rf_calibration_string_matches(profile->device_serial,
                                           context->device_serial) ||
        (profile->hardware_version != 0u &&
         profile->hardware_version != context->hardware_version)) {
        result->status = DAB_RF_CALIBRATION_DEVICE_MISMATCH;
        return result->status;
    }
    if ((profile->sample_rate_hz != 0u &&
         profile->sample_rate_hz != context->sample_rate_hz) ||
        (profile->bandwidth_hz != 0u &&
         profile->bandwidth_hz != context->bandwidth_hz) ||
        profile->if_mode != context->if_mode ||
        profile->antenna != context->antenna) {
        result->status = DAB_RF_CALIBRATION_SETTINGS_MISMATCH;
        return result->status;
    }

    for (index = 0u; index < profile->point_count; ++index) {
        const dab_rf_calibration_point_t *point = &profile->points[index];
        uint64_t const delta = dab_rf_calibration_frequency_delta(
            point->frequency_hz,
            context->frequency_hz);
        if (point->lna_state != context->lna_state ||
            point->gain_reduction_db != context->gain_reduction_db) {
            continue;
        }
        matching_gain_points++;
        if (delta <= profile->frequency_tolerance_hz &&
            delta < exact_delta) {
            exact = point;
            exact_delta = delta;
        }
        if (point->frequency_hz <= context->frequency_hz &&
            (lower == 0 || point->frequency_hz > lower->frequency_hz)) {
            lower = point;
        }
        if (point->frequency_hz >= context->frequency_hz &&
            (upper == 0 || point->frequency_hz < upper->frequency_hz)) {
            upper = point;
        }
    }

    if (matching_gain_points == 0u) {
        result->status = DAB_RF_CALIBRATION_GAIN_MISMATCH;
        return result->status;
    }
    if (exact != 0) {
        correction = exact->correction_db;
        uncertainty = exact->uncertainty_db;
        result->lower_frequency_hz = exact->frequency_hz;
        result->upper_frequency_hz = exact->frequency_hz;
    } else {
        double fraction;
        if (lower == 0 || upper == 0 ||
            upper->frequency_hz <= lower->frequency_hz) {
            result->status = DAB_RF_CALIBRATION_OUTSIDE_FREQUENCY_RANGE;
            return result->status;
        }
        fraction = (double)(context->frequency_hz - lower->frequency_hz) /
                   (double)(upper->frequency_hz - lower->frequency_hz);
        correction = lower->correction_db +
            (float)(fraction *
                    (double)(upper->correction_db - lower->correction_db));
        uncertainty = lower->uncertainty_db > upper->uncertainty_db
            ? lower->uncertainty_db
            : upper->uncertainty_db;
        result->lower_frequency_hz = lower->frequency_hz;
        result->upper_frequency_hz = upper->frequency_hz;
    }

    if (!isfinite(correction) || !isfinite(uncertainty)) {
        return result->status;
    }
    result->status = DAB_RF_CALIBRATION_VALID;
    result->valid = 1;
    result->correction_db = correction;
    result->uncertainty_db = uncertainty;
    result->calibrated_power_dbm = measured_power_dbfs + correction;
    return result->status;
}

const char *dab_rf_calibration_status_text(dab_rf_calibration_status_t status) {
    switch (status) {
    case DAB_RF_CALIBRATION_VALID:
        return "calibrated";
    case DAB_RF_CALIBRATION_NO_PROFILE:
        return "no profile";
    case DAB_RF_CALIBRATION_AGC_ACTIVE:
        return "AGC active";
    case DAB_RF_CALIBRATION_TELEMETRY_UNAVAILABLE:
        return "applied gain unavailable";
    case DAB_RF_CALIBRATION_DEVICE_MISMATCH:
        return "device mismatch";
    case DAB_RF_CALIBRATION_SETTINGS_MISMATCH:
        return "receiver settings mismatch";
    case DAB_RF_CALIBRATION_GAIN_MISMATCH:
        return "gain state not calibrated";
    case DAB_RF_CALIBRATION_OUTSIDE_FREQUENCY_RANGE:
        return "outside calibrated frequency range";
    default:
        return "invalid profile";
    }
}
