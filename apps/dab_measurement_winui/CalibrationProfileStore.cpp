#include "pch.h"
#include "CalibrationProfileStore.h"

#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string_view>

namespace
{
    template <size_t Capacity>
    void copy_text(char (&destination)[Capacity], std::string const& source)
    {
        size_t const count = (std::min)(source.size(), Capacity - 1u);
        std::memset(destination, 0, Capacity);
        std::memcpy(destination, source.data(), count);
    }

    std::string value_after_equals(std::string const& line)
    {
        size_t const equals = line.find('=');
        return equals == std::string::npos ? std::string{} : line.substr(equals + 1u);
    }

    bool parse_point(std::string const& value,
                     dab_rf_calibration_point_t& point)
    {
        std::istringstream input{ value };
        std::string field;
        try
        {
            if (!std::getline(input, field, ','))
            {
                return false;
            }
            point.frequency_hz = std::stoull(field);
            if (!std::getline(input, field, ','))
            {
                return false;
            }
            point.lna_state = static_cast<uint8_t>(std::stoul(field));
            if (!std::getline(input, field, ','))
            {
                return false;
            }
            point.gain_reduction_db = std::stoi(field);
            if (!std::getline(input, field, ','))
            {
                return false;
            }
            point.reference_power_dbm = std::stof(field);
            if (!std::getline(input, field, ','))
            {
                return false;
            }
            point.measured_power_dbfs = std::stof(field);
            if (!std::getline(input, field, ','))
            {
                return false;
            }
            point.uncertainty_db = std::stof(field);
        }
        catch (...)
        {
            return false;
        }
        return true;
    }
}

namespace dab::measurement
{
    std::filesystem::path CalibrationProfileStore::ProfilePath()
    {
        wchar_t local_app_data[32768]{};
        DWORD const length = GetEnvironmentVariableW(
            L"LOCALAPPDATA",
            local_app_data,
            static_cast<DWORD>(std::size(local_app_data)));
        std::filesystem::path root =
            length != 0u && length < std::size(local_app_data)
                ? std::filesystem::path{ local_app_data }
                : std::filesystem::temp_directory_path();
        return root / L"DabMeasurementWinUI" / L"rf-calibration-v1.profile";
    }

    bool CalibrationProfileStore::Load(
        dab_rf_calibration_profile_t& profile,
        std::wstring& message)
    {
        auto const path = ProfilePath();
        std::ifstream input{ path };
        dab_rf_calibration_profile_t loaded{};
        std::string line;

        dab_rf_calibration_profile_init(&loaded);
        if (!input)
        {
            message = L"No saved RF calibration profile";
            profile = loaded;
            return false;
        }

        try
        {
            while (std::getline(input, line))
            {
                if (line.empty() || line.front() == '#')
                {
                    continue;
                }
                std::string const value = value_after_equals(line);
                if (line.starts_with("version="))
                {
                    loaded.version = static_cast<uint32_t>(std::stoul(value));
                }
                else if (line.starts_with("id="))
                {
                    copy_text(loaded.id, value);
                }
                else if (line.starts_with("name="))
                {
                    copy_text(loaded.name, value);
                }
                else if (line.starts_with("driver="))
                {
                    copy_text(loaded.driver, value);
                }
                else if (line.starts_with("serial="))
                {
                    copy_text(loaded.device_serial, value);
                }
                else if (line.starts_with("hardware_version="))
                {
                    loaded.hardware_version =
                        static_cast<uint8_t>(std::stoul(value));
                }
                else if (line.starts_with("sample_rate_hz="))
                {
                    loaded.sample_rate_hz =
                        static_cast<uint32_t>(std::stoul(value));
                }
                else if (line.starts_with("bandwidth_hz="))
                {
                    loaded.bandwidth_hz =
                        static_cast<uint32_t>(std::stoul(value));
                }
                else if (line.starts_with("if_mode="))
                {
                    loaded.if_mode = static_cast<uint8_t>(std::stoul(value));
                }
                else if (line.starts_with("antenna="))
                {
                    loaded.antenna = static_cast<uint8_t>(std::stoul(value));
                }
                else if (line.starts_with("frequency_tolerance_hz="))
                {
                    loaded.frequency_tolerance_hz =
                        static_cast<uint32_t>(std::stoul(value));
                }
                else if (line.starts_with("default_uncertainty_db="))
                {
                    loaded.default_uncertainty_db = std::stof(value);
                }
                else if (line.starts_with("point="))
                {
                    dab_rf_calibration_point_t point{};
                    if (!parse_point(value, point) ||
                        dab_rf_calibration_profile_add_point(&loaded, &point) != 0)
                    {
                        message = L"The RF calibration profile contains an invalid point";
                        return false;
                    }
                }
            }
        }
        catch (...)
        {
            message = L"The RF calibration profile could not be parsed";
            return false;
        }

        if (loaded.version != DAB_RF_CALIBRATION_PROFILE_VERSION ||
            loaded.point_count == 0u)
        {
            message = L"The RF calibration profile is empty or incompatible";
            return false;
        }
        profile = loaded;
        message = L"Loaded RF calibration profile";
        return true;
    }

    bool CalibrationProfileStore::Save(
        dab_rf_calibration_profile_t const& profile,
        std::wstring& message)
    {
        auto const path = ProfilePath();
        auto temporary = path;
        temporary += L".tmp";

        std::error_code error;
        std::filesystem::create_directories(path.parent_path(), error);
        if (error)
        {
            message = L"Could not create the RF calibration profile folder";
            return false;
        }

        std::ofstream output{ temporary, std::ios::trunc };
        if (!output)
        {
            message = L"Could not create the RF calibration profile";
            return false;
        }
        output.precision(9);
        output << "# DAB+ Measurement Workbench RF calibration profile\n";
        output << "version=" << profile.version << '\n';
        output << "id=" << profile.id << '\n';
        output << "name=" << profile.name << '\n';
        output << "driver=" << profile.driver << '\n';
        output << "serial=" << profile.device_serial << '\n';
        output << "hardware_version="
               << static_cast<unsigned int>(profile.hardware_version) << '\n';
        output << "sample_rate_hz=" << profile.sample_rate_hz << '\n';
        output << "bandwidth_hz=" << profile.bandwidth_hz << '\n';
        output << "if_mode=" << static_cast<unsigned int>(profile.if_mode) << '\n';
        output << "antenna=" << static_cast<unsigned int>(profile.antenna) << '\n';
        output << "frequency_tolerance_hz="
               << profile.frequency_tolerance_hz << '\n';
        output << "default_uncertainty_db="
               << profile.default_uncertainty_db << '\n';
        for (uint32_t index = 0u; index < profile.point_count; ++index)
        {
            auto const& point = profile.points[index];
            output << "point="
                   << point.frequency_hz << ','
                   << static_cast<unsigned int>(point.lna_state) << ','
                   << point.gain_reduction_db << ','
                   << point.reference_power_dbm << ','
                   << point.measured_power_dbfs << ','
                   << point.uncertainty_db << '\n';
        }
        output.flush();
        if (!output)
        {
            message = L"Could not write the RF calibration profile";
            return false;
        }
        output.close();

        if (!MoveFileExW(temporary.c_str(),
                         path.c_str(),
                         MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        {
            std::filesystem::remove(temporary, error);
            message = L"Could not activate the RF calibration profile";
            return false;
        }
        message = L"Saved and activated RF calibration profile";
        return true;
    }
}
