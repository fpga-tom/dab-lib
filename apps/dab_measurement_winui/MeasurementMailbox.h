#pragma once

#include <array>
#include <cstdint>
#include <mutex>

#include "dab/receive.h"

namespace dab::measurement
{
    struct MeasurementFrame
    {
        uint64_t sequence{ 0 };
        uint64_t timestamp_ns{ 0 };
        uint64_t frequency_hz{ 0 };
        uint64_t frame_start_sample{ 0 };
        uint32_t sample_rate_hz{ 0 };
        uint32_t spectrum_bin_count{ 0 };
        float spectrum_start_hz{ 0.0f };
        float spectrum_bin_hz{ 0.0f };
        std::array<float, DAB_RECEIVE_SPECTRUM_BIN_COUNT> spectrum_dbfs{};
        uint32_t constellation_point_count{ 0 };
        std::array<float, DAB_RECEIVE_CONSTELLATION_POINT_CAPACITY> constellation_i{};
        std::array<float, DAB_RECEIVE_CONSTELLATION_POINT_CAPACITY> constellation_q{};
        uint32_t cir_bin_count{ 0 };
        float cir_sample_period_ns{ 0.0f };
        std::array<float, DAB_RECEIVE_CIR_BIN_COUNT> cir_power_db{};
        uint32_t channel_response_count{ 0 };
        std::array<float, DAB_RECEIVE_CHANNEL_RESPONSE_COUNT> channel_magnitude_db{};
        std::array<float, DAB_RECEIVE_CHANNEL_RESPONSE_COUNT> channel_phase_deg{};
        uint32_t carrier_mer_count{ 0 };
        int32_t first_carrier{ 0 };
        uint32_t negative_carrier_count{ 0 };
        float carrier_spacing_hz{ 0.0f };
        std::array<float, DAB_RECEIVE_CARRIER_MER_COUNT> carrier_mer_db{};
    };

    class MeasurementMailbox
    {
    public:
        static int Callback(dab_receive_measurement_frame_t const* frame, void* user) noexcept;

        void Publish(dab_receive_measurement_frame_t const& frame) noexcept;
        bool TryReadLatest(uint64_t previous_version,
                           MeasurementFrame& frame,
                           uint64_t& version) const noexcept;

    private:
        mutable std::mutex m_mutex;
        MeasurementFrame m_latest{};
        uint64_t m_version{ 0 };
        bool m_valid{ false };
    };
}
