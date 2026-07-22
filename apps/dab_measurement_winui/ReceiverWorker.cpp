#include "pch.h"
#include "ReceiverWorker.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <limits>
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
        ResetPlaybackState();
        m_recovery_attempt_count = 0u;
        m_snapshot_sequence_offset = 0u;
        m_measurement_sequence_offset = 0u;
        m_blocks_received_offset = 0u;
        m_dropped_blocks_offset = 0u;
        m_processing_late_offset = 0u;
        m_hardware_overruns_offset = 0u;
        m_last_session_snapshot = {};
        m_last_session_measurement_sequence = 0u;
        m_last_session_snapshot_valid = false;
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
        if (state == ReceiverState::starting ||
            state == ReceiverState::running ||
            state == ReceiverState::recovering)
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
               state == ReceiverState::recovering ||
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

    int ReceiverWorker::SnapshotCallback(dab_receive_snapshot_t const* snapshot,
                                         void* user) noexcept
    {
        if (snapshot == nullptr || user == nullptr)
        {
            return -1;
        }

        auto* worker = static_cast<ReceiverWorker*>(user);
        dab_receive_snapshot_t adjusted = *snapshot;
        auto const saturate_u32 = [](uint64_t value) noexcept
        {
            return static_cast<uint32_t>((std::min)(
                value,
                static_cast<uint64_t>((std::numeric_limits<uint32_t>::max)())));
        };

        worker->m_last_session_snapshot = *snapshot;
        worker->m_last_session_snapshot_valid = true;
        adjusted.sequence += worker->m_snapshot_sequence_offset;
        adjusted.blocks_received += worker->m_blocks_received_offset;
        adjusted.dropped_blocks = saturate_u32(
            worker->m_dropped_blocks_offset + snapshot->dropped_blocks);
        adjusted.processing_late_count = saturate_u32(
            worker->m_processing_late_offset + snapshot->processing_late_count);
        if (snapshot->hardware_overruns_valid != 0)
        {
            adjusted.hardware_overruns = saturate_u32(
                worker->m_hardware_overruns_offset + snapshot->hardware_overruns);
        }
        worker->m_mailbox.Publish(adjusted);
        return 0;
    }

    int ReceiverWorker::MeasurementCallback(dab_receive_measurement_frame_t const* frame,
                                            void* user) noexcept
    {
        if (frame == nullptr || user == nullptr)
        {
            return -1;
        }

        auto* worker = static_cast<ReceiverWorker*>(user);
        dab_receive_measurement_frame_t adjusted = *frame;
        worker->m_last_session_measurement_sequence = frame->sequence;
        adjusted.sequence += worker->m_measurement_sequence_offset;
        worker->m_measurement_mailbox.Publish(adjusted);
        return 0;
    }

    int ReceiverWorker::OnEvent(dab_receive_event_t const& event) noexcept
    {
        try
        {
            if (event.type == DAB_RECEIVE_EVENT_ERROR)
            {
                PublishStatus(State(), event.message != nullptr ? event.message : "Receiver error", true);
            }
            else if (event.type == DAB_RECEIVE_EVENT_STREAM_STALLED ||
                     event.type == DAB_RECEIVE_EVENT_DECODER_STALLED)
            {
                PublishStatus(ReceiverState::recovering,
                              event.message != nullptr
                                  ? event.message
                                  : "Receiver progress stalled; measurements are stale",
                              false,
                              event.type == DAB_RECEIVE_EVENT_DECODER_STALLED
                                  ? DAB_RECEIVE_RESULT_DECODER_STALLED
                                  : DAB_RECEIVE_RESULT_STREAM_STALLED);
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
                else if (state == ReceiverState::recovering &&
                         message.starts_with("Receiver started"))
                {
                    state = ReceiverState::running;
                    m_recovery_attempt_count = 0u;
                    message = "SDR stream recovered; " + message;
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
        config.snapshot = &ReceiverWorker::SnapshotCallback;
        config.snapshot_user = this;
        config.measurement = &ReceiverWorker::MeasurementCallback;
        config.measurement_user = this;
        config.measurement_decimation = settings.measurement_decimation;
        config.stream_watchdog_timeout_ms = settings.stream_watchdog_timeout_ms;
        config.decoder_watchdog_timeout_ms = settings.decoder_watchdog_timeout_ms;
        config.catalog = &CatalogMailbox::Callback;
        config.catalog_user = &m_catalog_mailbox;

        for (;;)
        {
            int const result = dab_receive_service(&config, &ReceiverWorker::EventCallback, this);
            dab_audio_playback_close(&m_audio_playback);
            ResetPlaybackState();

            if (m_cancel_requested.load(std::memory_order_acquire))
            {
                PublishStatus(ReceiverState::stopped, "Receiver stopped");
                return;
            }
            if (result == 0)
            {
                PublishStatus(ReceiverState::stopped, "Receiver completed");
                return;
            }
            if (result == DAB_RECEIVE_RESULT_STREAM_STALLED ||
                result == DAB_RECEIVE_RESULT_DECODER_STALLED ||
                State() == ReceiverState::recovering)
            {
                AccumulateRecoveredSession();
                ++m_recovery_attempt_count;
                uint32_t const shift = (std::min)(m_recovery_attempt_count - 1u, 3u);
                uint32_t const backoff_ms = (std::min)(250u << shift, 2000u);
                char message[144]{};
                std::snprintf(message,
                              sizeof(message),
                              "Measurements stale; restarting SDR stream (attempt %u in %u ms)",
                              m_recovery_attempt_count,
                              backoff_ms);
                PublishStatus(ReceiverState::recovering,
                              message,
                              false,
                              DAB_RECEIVE_RESULT_STREAM_STALLED);

                uint32_t waited_ms = 0u;
                while (waited_ms < backoff_ms &&
                       !m_cancel_requested.load(std::memory_order_acquire))
                {
                    uint32_t const slice_ms = (std::min)(50u, backoff_ms - waited_ms);
                    std::this_thread::sleep_for(std::chrono::milliseconds(slice_ms));
                    waited_ms += slice_ms;
                }
                continue;
            }

            char message[96]{};
            std::snprintf(message, sizeof(message), "Receiver stopped with error code %d", result);
            PublishStatus(ReceiverState::failed, message, true, result);
            return;
        }
    }

    void ReceiverWorker::ResetPlaybackState() noexcept
    {
        m_audio_format_status_valid = false;
        m_audio_format_service_id = 0u;
        m_audio_format_frequency_hz = 0u;
        m_audio_format_sample_rate = 0u;
        m_audio_format_channels = 0u;
        m_audio_format_asc.fill(0u);
        m_audio_format_asc_length = 0u;
        m_audio_playback_failed = false;
    }

    void ReceiverWorker::AccumulateRecoveredSession() noexcept
    {
        if (m_last_session_snapshot_valid)
        {
            m_snapshot_sequence_offset += m_last_session_snapshot.sequence;
            m_blocks_received_offset += m_last_session_snapshot.blocks_received;
            m_dropped_blocks_offset += m_last_session_snapshot.dropped_blocks;
            m_processing_late_offset += m_last_session_snapshot.processing_late_count;
            if (m_last_session_snapshot.hardware_overruns_valid != 0)
            {
                m_hardware_overruns_offset += m_last_session_snapshot.hardware_overruns;
            }
        }
        m_measurement_sequence_offset += m_last_session_measurement_sequence;
        m_last_session_snapshot = {};
        m_last_session_measurement_sequence = 0u;
        m_last_session_snapshot_valid = false;
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
