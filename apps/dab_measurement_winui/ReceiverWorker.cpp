#include "pch.h"
#include "ReceiverWorker.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <limits>
#include <optional>
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
        StopAudioWorker();

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
        if (!StartAudioWorker())
        {
            PublishStatus(ReceiverState::failed, "Could not create audio worker thread", true, -1);
            return false;
        }
        PublishStatus(ReceiverState::starting, "Opening receiver...");
        try
        {
            m_thread = std::thread{ &ReceiverWorker::Run, this, std::move(settings) };
        }
        catch (...)
        {
            StopAudioWorker();
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
            StopAudioWorker();
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
        StopAudioWorker();
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
                EnqueueAudioFormat(event);

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
                     m_audio_format_status_valid)
            {
                EnqueueAudioAccessUnit(event);
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
            EnqueueAudioReset();
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
                if (m_cancel_requested.load(std::memory_order_acquire))
                {
                    PublishStatus(ReceiverState::stopped, "Receiver stopped");
                    return;
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
    }

    bool ReceiverWorker::StartAudioWorker()
    {
        {
            std::scoped_lock lock{ m_audio_queue_mutex };
            m_audio_stop_requested = false;
            m_audio_commands.clear();
        }
        try
        {
            m_audio_thread = std::thread{ &ReceiverWorker::AudioThreadMain, this };
        }
        catch (...)
        {
            return false;
        }
        return true;
    }

    void ReceiverWorker::StopAudioWorker() noexcept
    {
        {
            std::scoped_lock lock{ m_audio_queue_mutex };
            m_audio_stop_requested = true;
            m_audio_commands.clear();
        }
        m_audio_queue_cv.notify_all();
        if (m_audio_thread.joinable())
        {
            m_audio_thread.join();
        }
    }

    void ReceiverWorker::EnqueueAudioFormat(dab_receive_event_t const& event)
    {
        AudioCommand command;
        command.kind = AudioCommandKind::format;
        command.service_id = event.service_id;
        command.frequency_hz = event.frequency_hz;
        command.sample_rate = event.sample_rate;
        command.channels = event.channels;
        command.asc_length = (std::min)(
            event.asc_length,
            static_cast<uint32_t>(command.asc.size()));
        if (event.asc != nullptr && command.asc_length != 0u)
        {
            std::copy_n(event.asc, command.asc_length, command.asc.begin());
        }

        {
            std::scoped_lock lock{ m_audio_queue_mutex };
            if (m_audio_stop_requested)
            {
                return;
            }
            m_audio_commands.clear();
            m_audio_commands.push_back(std::move(command));
        }
        m_audio_queue_cv.notify_one();
    }

    void ReceiverWorker::EnqueueAudioAccessUnit(dab_receive_event_t const& event)
    {
        constexpr size_t audio_queue_capacity = 256u;
        AudioCommand command;
        if (event.audio_data == nullptr || event.audio_length == 0u)
        {
            return;
        }
        command.kind = AudioCommandKind::access_unit;
        command.generation = event.generation;
        command.data.assign(event.audio_data, event.audio_data + event.audio_length);

        {
            std::scoped_lock lock{ m_audio_queue_mutex };
            if (m_audio_stop_requested)
            {
                return;
            }
            while (m_audio_commands.size() >= audio_queue_capacity)
            {
                auto const oldest_audio = std::find_if(
                    m_audio_commands.begin(),
                    m_audio_commands.end(),
                    [](AudioCommand const& queued)
                    {
                        return queued.kind == AudioCommandKind::access_unit;
                    });
                if (oldest_audio == m_audio_commands.end())
                {
                    return;
                }
                m_audio_commands.erase(oldest_audio);
            }
            m_audio_commands.push_back(std::move(command));
        }
        m_audio_queue_cv.notify_one();
    }

    void ReceiverWorker::EnqueueAudioReset() noexcept
    {
        try
        {
            AudioCommand command;
            command.kind = AudioCommandKind::reset;
            {
                std::scoped_lock lock{ m_audio_queue_mutex };
                if (m_audio_stop_requested)
                {
                    return;
                }
                m_audio_commands.clear();
                m_audio_commands.push_back(std::move(command));
            }
            m_audio_queue_cv.notify_one();
        }
        catch (...)
        {
        }
    }

    void ReceiverWorker::AudioThreadMain() noexcept
    {
        using clock = std::chrono::steady_clock;
        std::optional<AudioCommand> active_format;
        bool decoder_configured = false;
        bool recovering = false;
        uint32_t retry_attempt = 0u;
        auto retry_at = clock::time_point{};

        auto schedule_retry = [&](char const* reason)
        {
            dab_audio_playback_close(&m_audio_playback);
            decoder_configured = false;
            recovering = true;
            ++retry_attempt;
            uint32_t const shift = (std::min)(retry_attempt - 1u, 3u);
            uint32_t const retry_ms = (std::min)(250u << shift, 2000u);
            retry_at = clock::now() + std::chrono::milliseconds(retry_ms);
            char message[160]{};
            std::snprintf(message,
                          sizeof(message),
                          "%s; retrying audio independently in %u ms",
                          reason,
                          retry_ms);
            PublishStatus(State(), message);
        };

        try
        {
            for (;;)
            {
                std::optional<AudioCommand> command;
                {
                    std::unique_lock lock{ m_audio_queue_mutex };
                    m_audio_queue_cv.wait_for(
                        lock,
                        std::chrono::milliseconds(100),
                        [&]
                        {
                            return m_audio_stop_requested || !m_audio_commands.empty();
                        });
                    if (m_audio_stop_requested)
                    {
                        break;
                    }
                    if (!m_audio_commands.empty())
                    {
                        command = std::move(m_audio_commands.front());
                        m_audio_commands.pop_front();
                    }
                }

                if (command && command->kind == AudioCommandKind::reset)
                {
                    dab_audio_playback_close(&m_audio_playback);
                    active_format.reset();
                    decoder_configured = false;
                    recovering = false;
                    retry_attempt = 0u;
                    retry_at = clock::time_point{};
                    continue;
                }
                if (command && command->kind == AudioCommandKind::format)
                {
                    dab_audio_playback_close(&m_audio_playback);
                    active_format = std::move(*command);
                    decoder_configured = false;
                    recovering = false;
                    retry_attempt = 0u;
                    retry_at = clock::time_point{};
                    continue;
                }
                if (command && command->kind == AudioCommandKind::access_unit && active_format)
                {
                    auto const now = clock::now();
                    if (!decoder_configured && now >= retry_at)
                    {
                        int const configure_result = dab_audio_playback_set_format(
                            &m_audio_playback,
                            active_format->service_id,
                            active_format->sample_rate,
                            active_format->channels,
                            active_format->asc.data(),
                            active_format->asc_length);
                        if (configure_result != 0)
                        {
                            schedule_retry("Audio decoder or output setup failed");
                            continue;
                        }
                        decoder_configured = true;
                    }
                    if (!decoder_configured)
                    {
                        continue;
                    }

                    int const playback_result = dab_audio_playback_consume_au(
                        &m_audio_playback,
                        command->data.data(),
                        static_cast<uint32_t>(command->data.size()),
                        command->generation);
                    if (playback_result < 0)
                    {
                        schedule_retry("Audio decoding or output failed");
                        continue;
                    }
                    if (playback_result == 0 &&
                        dab_audio_playback_output_active(&m_audio_playback) > 0 &&
                        recovering)
                    {
                        recovering = false;
                        retry_attempt = 0u;
                        PublishStatus(State(), "Audio output recovered without restarting the receiver");
                    }
                }

                if (decoder_configured && m_audio_playback.stream != nullptr)
                {
                    int const active = dab_audio_playback_output_active(&m_audio_playback);
                    if (active <= 0)
                    {
                        schedule_retry("Audio output device stopped");
                    }
                }
            }
        }
        catch (...)
        {
            PublishStatus(State(), "Audio worker stopped after an internal error", true);
        }
        dab_audio_playback_close(&m_audio_playback);
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
