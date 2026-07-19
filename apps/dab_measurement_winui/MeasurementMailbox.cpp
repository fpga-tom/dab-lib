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
