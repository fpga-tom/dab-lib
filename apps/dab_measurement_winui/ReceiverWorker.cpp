#include "pch.h"
#include "ReceiverWorker.h"

#include <cstdio>
#include <cstring>
#include <utility>

namespace dab::measurement
{
    ReceiverWorker::ReceiverWorker(SnapshotMailbox& mailbox,
                                   MeasurementMailbox& measurement_mailbox,
                                   CatalogMailbox& catalog_mailbox) noexcept
        : m_mailbox(mailbox),
          m_measurement_mailbox(measurement_mailbox),
          m_catalog_mailbox(catalog_mailbox)
    {
    }

    ReceiverWorker::~ReceiverWorker()
    {
        StopAndJoin();
    }

    bool ReceiverWorker::Start(ReceiverSettings settings)
    {
        std::scoped_lock lock{ m_lifecycle_mutex };
        if (IsActive())
        {
            return false;
        }
        if (m_thread.joinable())
        {
            m_thread.join();
        }

        m_cancel_requested.store(false, std::memory_order_release);
        m_audio_format_status_valid = false;
        m_audio_format_service_id = 0u;
        m_audio_format_frequency_hz = 0u;
        m_audio_format_sample_rate = 0u;
        m_audio_format_channels = 0u;
        m_audio_format_asc.fill(0u);
        m_audio_format_asc_length = 0u;
        m_audio_playback_failed = false;
        PublishStatus(ReceiverState::starting, "Opening receiver...");
        try
        {
            m_thread = std::thread{ &ReceiverWorker::Run, this, std::move(settings) };
        }
        catch (...)
        {
            PublishStatus(ReceiverState::failed, "Could not create receiver worker thread", true, -1);
            return false;
        }
        return true;
    }

    void ReceiverWorker::RequestStop() noexcept
    {
        m_cancel_requested.store(true, std::memory_order_release);
        ReceiverState const state = State();
        if (state == ReceiverState::starting || state == ReceiverState::running)
        {
            PublishStatus(ReceiverState::stopping, "Stopping receiver...");
        }
    }

    void ReceiverWorker::JoinFinished()
    {
        std::scoped_lock lock{ m_lifecycle_mutex };
        if (m_thread.joinable() && !IsActive())
        {
            m_thread.join();
        }
    }

    void ReceiverWorker::StopAndJoin() noexcept
    {
        RequestStop();
        std::scoped_lock lock{ m_lifecycle_mutex };
        if (m_thread.joinable())
        {
            m_thread.join();
        }
    }

    ReceiverState ReceiverWorker::State() const noexcept
    {
        return m_state.load(std::memory_order_acquire);
    }

    bool ReceiverWorker::IsActive() const noexcept
    {
        ReceiverState const state = State();
        return state == ReceiverState::starting ||
               state == ReceiverState::running ||
               state == ReceiverState::stopping;
    }

    bool ReceiverWorker::TryReadStatus(uint64_t after_version,
                                       ReceiverStatus& status,
                                       uint64_t& version) const
    {
        std::scoped_lock lock{ m_status_mutex };
        if (m_status_version == after_version)
        {
            return false;
        }
        status = m_latest_status;
        version = m_status_version;
        return true;
    }

    int ReceiverWorker::CancelCallback(void* user) noexcept
    {
        if (user == nullptr)
        {
            return 1;
        }
        auto const* worker = static_cast<ReceiverWorker const*>(user);
        return worker->m_cancel_requested.load(std::memory_order_acquire) ? 1 : 0;
    }

    int ReceiverWorker::EventCallback(dab_receive_event_t const* event, void* user) noexcept
    {
        if (event == nullptr || user == nullptr)
        {
            return 0;
        }
        return static_cast<ReceiverWorker*>(user)->OnEvent(*event);
    }

    int ReceiverWorker::OnEvent(dab_receive_event_t const& event) noexcept
    {
        try
        {
            if (event.type == DAB_RECEIVE_EVENT_ERROR)
            {
                PublishStatus(State(), event.message != nullptr ? event.message : "Receiver error", true);
            }
            else if (event.type == DAB_RECEIVE_EVENT_STATUS)
            {
                ReceiverState state = State();
                std::string message = event.message != nullptr ? event.message : "Receiver status changed";
                if (state == ReceiverState::starting &&
                    (message.starts_with("Warming up") || message.starts_with("Receiver started")))
                {
                    state = ReceiverState::running;
                }
                PublishStatus(state, std::move(message));
            }
            else if (event.type == DAB_RECEIVE_EVENT_AUDIO_FORMAT)
            {
                uint32_t const asc_length = (std::min)(
                    event.asc_length,
                    static_cast<uint32_t>(m_audio_format_asc.size()));
                bool const same_format = m_audio_format_status_valid &&
                    m_audio_format_service_id == event.service_id &&
                    m_audio_format_frequency_hz == event.frequency_hz &&
                    m_audio_format_sample_rate == event.sample_rate &&
                    m_audio_format_channels == event.channels &&
                    m_audio_format_asc_length == event.asc_length &&
                    (asc_length == 0u ||
                     (event.asc != nullptr &&
                      std::memcmp(m_audio_format_asc.data(), event.asc, asc_length) == 0));
                if (same_format)
                {
                    return 0;
                }

                m_audio_format_status_valid = true;
                m_audio_format_service_id = event.service_id;
                m_audio_format_frequency_hz = event.frequency_hz;
                m_audio_format_sample_rate = event.sample_rate;
                m_audio_format_channels = event.channels;
                m_audio_format_asc.fill(0u);
                if (event.asc != nullptr && asc_length != 0u)
                {
                    std::memcpy(m_audio_format_asc.data(), event.asc, asc_length);
                }
                m_audio_format_asc_length = event.asc_length;

                if (dab_audio_playback_set_format(&m_audio_playback,
                                                  event.service_id,
                                                  event.sample_rate,
                                                  event.channels,
                                                  event.asc,
                                                  event.asc_length) != 0)
                {
                    m_audio_playback_failed = true;
                    PublishStatus(ReceiverState::running,
                                  "DAB+ audio decoded, but playback could not be opened",
                                  true);
                    return 0;
                }

                char message[96]{};
                std::snprintf(message,
                              sizeof(message),
                              "DAB+ audio available: %u Hz, %u channel%s",
                              event.sample_rate,
                              event.channels,
                              event.channels == 1u ? "" : "s");
                PublishStatus(ReceiverState::running, message);
            }
            else if (event.type == DAB_RECEIVE_EVENT_AUDIO_AU &&
                     m_audio_format_status_valid &&
                     !m_audio_playback_failed)
            {
                int const playback_result = dab_audio_playback_consume_au(
                    &m_audio_playback,
                    event.audio_data,
                    event.audio_length,
                    event.generation);
                if (playback_result < 0)
                {
                    dab_audio_playback_close(&m_audio_playback);
                    m_audio_playback_failed = true;
                    PublishStatus(ReceiverState::running,
                                  "DAB+ audio playback stopped after an output error",
                                  true);
                }
            }
        }
        catch (...)
        {
            return 0;
        }
        return 0;
    }

    void ReceiverWorker::Run(ReceiverSettings settings) noexcept
    {
        dab_receive_config_t config{};
        dab_receive_default_config(&config);
        config.driver = settings.driver.c_str();
        config.frequency_hz = settings.frequency_hz;
        config.service_id = settings.service_id;
        config.gain_db = settings.gain_db;
        config.agc_enabled = settings.agc_enabled ? 1 : 0;
        config.ppm_correction = settings.ppm_correction;
        config.bias_tee_enabled = settings.bias_tee_enabled ? 1 : 0;
        config.use_real_device = settings.use_real_device ? 1 : 0;
        config.cancel = &ReceiverWorker::CancelCallback;
        config.cancel_user = this;
        config.snapshot = &SnapshotMailbox::Callback;
        config.snapshot_user = &m_mailbox;
        config.measurement = &MeasurementMailbox::Callback;
        config.measurement_user = &m_measurement_mailbox;
        config.measurement_decimation = settings.measurement_decimation;
        config.catalog = &CatalogMailbox::Callback;
        config.catalog_user = &m_catalog_mailbox;

        int const result = dab_receive_service(&config, &ReceiverWorker::EventCallback, this);
        dab_audio_playback_close(&m_audio_playback);
        if (m_cancel_requested.load(std::memory_order_acquire))
        {
            PublishStatus(ReceiverState::stopped, "Receiver stopped");
        }
        else if (result == 0)
        {
            PublishStatus(ReceiverState::stopped, "Receiver completed");
        }
        else
        {
            char message[96]{};
            std::snprintf(message, sizeof(message), "Receiver stopped with error code %d", result);
            PublishStatus(ReceiverState::failed, message, true, result);
        }
    }

    void ReceiverWorker::PublishStatus(ReceiverState state,
                                       std::string message,
                                       bool is_error,
                                       int result_code) noexcept
    {
        m_state.store(state, std::memory_order_release);
        try
        {
            std::scoped_lock lock{ m_status_mutex };
            m_latest_status.state = state;
            m_latest_status.message = std::move(message);
            m_latest_status.result_code = result_code;
            m_latest_status.is_error = is_error;
            ++m_status_version;
        }
        catch (...)
        {
        }
    }
}
