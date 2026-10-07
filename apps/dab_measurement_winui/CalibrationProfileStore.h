#pragma once

#include "dab/rf_calibration.h"

#include <filesystem>
#include <string>

namespace dab::measurement
{
    class CalibrationProfileStore final
    {
    public:
        [[nodiscard]] static std::filesystem::path ProfilePath();
        static bool Load(dab_rf_calibration_profile_t& profile,
                         std::wstring& message);
        static bool Save(dab_rf_calibration_profile_t const& profile,
                         std::wstring& message);
    };
}
