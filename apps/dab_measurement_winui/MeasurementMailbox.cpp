#include "pch.h"
#include "MeasurementMailbox.h"

#include <algorithm>

namespace dab::measurement
{
    int MeasurementMailbox::Callback(dab_receive_measurement_frame_t const* frame, void* user) noexcept
    {
        if (frame == nullptr || user == nullptr)
        {
            return -1;
        }
        static_cast<MeasurementMailbox*>(user)->Publish(*frame);
        return 0;
    }

    void MeasurementMailbox::Publish(dab_receive_measurement_frame_t const& source) noexcept
    {
        try
        {
            std::scoped_lock lock{ m_mutex };
            m_latest.sequence = source.sequence;
            m_latest.timestamp_ns = source.timestamp_ns;
            m_latest.frequency_hz = source.frequency_hz;
            m_latest.frame_start_sample = source.frame_start_sample;
            m_latest.sample_rate_hz = source.sample_rate_hz;
            m_latest.spectrum_bin_count = (std::min)(
                source.spectrum_bin_count,
                static_cast<uint32_t>(DAB_RECEIVE_SPECTRUM_BIN_COUNT));
            m_latest.spectrum_start_hz = source.spectrum_start_hz;
            m_latest.spectrum_bin_hz = source.spectrum_bin_hz;
            if (source.spectrum_dbfs != nullptr)
            {
                std::copy_n(source.spectrum_dbfs,
                            m_latest.spectrum_bin_count,
                            m_latest.spectrum_dbfs.begin());
            }
            m_latest.constellation_point_count = (std::min)(
                source.constellation_point_count,
                static_cast<uint32_t>(DAB_RECEIVE_CONSTELLATION_POINT_CAPACITY));
            if (source.constellation_i != nullptr && source.constellation_q != nullptr)
            {
                std::copy_n(source.constellation_i,
                            m_latest.constellation_point_count,
                            m_latest.constellation_i.begin());
                std::copy_n(source.constellation_q,
                            m_latest.constellation_point_count,
                            m_latest.constellation_q.begin());
            }
            m_latest.cir_bin_count = (std::min)(
                source.cir_bin_count,
                static_cast<uint32_t>(DAB_RECEIVE_CIR_BIN_COUNT));
            m_latest.cir_sample_period_ns = source.cir_sample_period_ns;
            if (source.cir_power_db != nullptr)
            {
                std::copy_n(source.cir_power_db,
                            m_latest.cir_bin_count,
                            m_latest.cir_power_db.begin());
            }
            m_latest.channel_response_count = (std::min)(
                source.channel_response_count,
                static_cast<uint32_t>(DAB_RECEIVE_CHANNEL_RESPONSE_COUNT));
            if (source.channel_magnitude_db != nullptr && source.channel_phase_deg != nullptr)
            {
                std::copy_n(source.channel_magnitude_db,
                            m_latest.channel_response_count,
                            m_latest.channel_magnitude_db.begin());
                std::copy_n(source.channel_phase_deg,
                            m_latest.channel_response_count,
                            m_latest.channel_phase_deg.begin());
            }
            m_latest.carrier_mer_count = (std::min)(
                source.carrier_mer_count,
                static_cast<uint32_t>(DAB_RECEIVE_CARRIER_MER_COUNT));
            m_latest.first_carrier = source.first_carrier;
            m_latest.negative_carrier_count = source.negative_carrier_count;
            m_latest.carrier_spacing_hz = source.carrier_spacing_hz;
            if (source.carrier_mer_db != nullptr)
            {
                std::copy_n(source.carrier_mer_db,
                            m_latest.carrier_mer_count,
                            m_latest.carrier_mer_db.begin());
            }
            m_latest.null_scope_sample_count = (std::min)(
                source.null_scope_sample_count,
                static_cast<uint32_t>(DAB_RECEIVE_NULL_SCOPE_SAMPLE_CAPACITY));
            m_latest.null_scope_start_offset_samples = source.null_scope_start_offset_samples;
            m_latest.null_scope_start_index = source.null_scope_start_index;
            m_latest.null_scope_end_index = source.null_scope_end_index;
            m_latest.null_scope_duration_samples = source.null_scope_duration_samples;
            m_latest.null_scope_sample_period_ns = source.null_scope_sample_period_ns;
            m_latest.null_scope_threshold_dbfs = source.null_scope_threshold_dbfs;
            m_latest.null_scope_depth_db = source.null_scope_depth_db;
            m_latest.null_scope_confidence = source.null_scope_confidence;
            m_latest.null_scope_energy_dbfs = source.null_scope_energy_dbfs;
            if (source.null_scope_iq_interleaved != nullptr &&
                source.null_scope_power_dbfs != nullptr)
            {
                std::copy_n(source.null_scope_iq_interleaved,
                            2u * m_latest.null_scope_sample_count,
                            m_latest.null_scope_iq_interleaved.begin());
                std::copy_n(source.null_scope_power_dbfs,
                            m_latest.null_scope_sample_count,
                            m_latest.null_scope_power_dbfs.begin());
            }
            m_latest.null_scope_frame_bin_count = (std::min)(
                source.null_scope_frame_bin_count,
                static_cast<uint32_t>(DAB_RECEIVE_NULL_SCOPE_FRAME_BIN_COUNT));
            m_latest.null_scope_frame_bin_samples = source.null_scope_frame_bin_samples;
            m_latest.null_scope_frame_sample_count = source.null_scope_frame_sample_count;
            if (source.null_scope_frame_power_dbfs != nullptr)
            {
                std::copy_n(source.null_scope_frame_power_dbfs,
                            m_latest.null_scope_frame_bin_count,
                            m_latest.null_scope_frame_power_dbfs.begin());
            }
            ++m_version;
            m_valid = true;
        }
        catch (...)
        {
        }
    }

    bool MeasurementMailbox::TryReadLatest(uint64_t previous_version,
                                           MeasurementFrame& frame,
                                           uint64_t& version) const noexcept
    {
        try
        {
            std::scoped_lock lock{ m_mutex };
            if (!m_valid || m_version == previous_version)
            {
                return false;
            }
            frame = m_latest;
            version = m_version;
            return true;
        }
        catch (...)
        {
            return false;
        }
    }
}
