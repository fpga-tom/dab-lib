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
        ResetAudioMeasurements();
        m_recovery_attempt_count = 0u;
        m_snapshot_sequence_offset = 0u;
        m_measurement_sequence_offset = 0u;
        m_blocks_received_offset = 0u;
        m_dropped_blocks_offset = 0u;
        m_processing_late_offset = 0u;
        m_hardware_overruns_offset = 0u;
        m_fic_frames_offset = 0u;
        m_fic_valid_fibs_offset = 0u;
        m_fic_figs_offset = 0u;
        m_fic_fig_errors_offset = 0u;
        m_fic_pre_viterbi_error_bits_offset = 0u;
        m_fic_pre_viterbi_compared_bits_offset = 0u;
        m_fic_fig_count_offsets.fill(0u);
        m_fic_fig_change_offsets.fill(0u);
        m_fic_fig_malformed_offsets.fill(0u);
        m_msc_frames_offset = 0u;
        m_msc_generation_offset = 0u;
        m_msc_decode_attempts_offset = 0u;
        m_msc_decode_failures_offset = 0u;
        m_msc_firecode_checks_offset = 0u;
        m_msc_firecode_errors_offset = 0u;
        m_msc_rs_corrected_offset = 0u;
        m_msc_rs_corrected_superframes_offset = 0u;
        m_msc_rs_uncorrectable_offset = 0u;
        m_msc_au_crc_errors_offset = 0u;
        m_msc_au_crc_checks_offset = 0u;
        m_msc_ber_error_bits_offset = 0u;
        m_msc_ber_compared_bits_offset = 0u;
        m_msc_input_confidence_sum_offset = 0u;
        m_msc_input_confidence_samples_offset = 0u;
        m_msc_deinterleaver_cifs_offset = 0u;
        m_msc_cif_attempts_offset = 0u;
        m_msc_cif_successes_offset = 0u;
        m_msc_cif_failures_offset = 0u;
        m_msc_output_bytes_offset = 0u;
        m_msc_cif_discontinuities_offset = 0u;
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

    bool ReceiverWorker::AudioOutputActive() const noexcept
    {
        return m_audio_output_active.load(std::memory_order_acquire);
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
        adjusted.fic_frames_seen = saturate_u32(
            worker->m_fic_frames_offset + snapshot->fic_frames_seen);
        adjusted.fic_total_valid_fibs = saturate_u32(
            worker->m_fic_valid_fibs_offset + snapshot->fic_total_valid_fibs);
        adjusted.fic_total_figs += worker->m_fic_figs_offset;
        adjusted.fic_total_fig_parse_errors += worker->m_fic_fig_errors_offset;
        adjusted.fic_pre_viterbi_error_bits +=
            worker->m_fic_pre_viterbi_error_bits_offset;
        adjusted.fic_pre_viterbi_compared_bits +=
            worker->m_fic_pre_viterbi_compared_bits_offset;
        adjusted.fic_pre_viterbi_ber_valid =
            adjusted.fic_pre_viterbi_compared_bits != 0u;
        adjusted.fic_pre_viterbi_ber = adjusted.fic_pre_viterbi_ber_valid
            ? static_cast<float>(
                static_cast<double>(adjusted.fic_pre_viterbi_error_bits) /
                static_cast<double>(adjusted.fic_pre_viterbi_compared_bits))
            : 0.0f;
        for (uint32_t index = 0u;
             index < adjusted.fic_fig_stat_count;
             ++index)
        {
            auto& stat = adjusted.fic_fig_stats[index];
            size_t const key =
                (static_cast<size_t>(stat.type & 0x07u) << 5u) |
                static_cast<size_t>(stat.extension & 0x1Fu);
            if (stat.total_count != 0u)
            {
                stat.last_seen_frame = saturate_u32(
                    worker->m_fic_frames_offset + stat.last_seen_frame);
            }
            if (stat.change_count != 0u)
            {
                stat.last_change_frame = saturate_u32(
                    worker->m_fic_frames_offset + stat.last_change_frame);
            }
            stat.total_count += worker->m_fic_fig_count_offsets[key];
            stat.change_count += worker->m_fic_fig_change_offsets[key];
            stat.malformed_count += worker->m_fic_fig_malformed_offsets[key];
        }
        adjusted.msc_frames_seen = saturate_u32(
            worker->m_msc_frames_offset + snapshot->msc_frames_seen);
        adjusted.msc_superframe_generation += worker->m_msc_generation_offset;
        adjusted.msc_decode_attempts = saturate_u32(
            worker->m_msc_decode_attempts_offset + snapshot->msc_decode_attempts);
        adjusted.msc_decode_failures = saturate_u32(
            worker->m_msc_decode_failures_offset + snapshot->msc_decode_failures);
        adjusted.msc_firecode_checks_total = saturate_u32(
            worker->m_msc_firecode_checks_offset +
            snapshot->msc_firecode_checks_total);
        adjusted.msc_firecode_errors_total = saturate_u32(
            worker->m_msc_firecode_errors_offset +
            snapshot->msc_firecode_errors_total);
        adjusted.msc_rs_corrected_total = saturate_u32(
            worker->m_msc_rs_corrected_offset + snapshot->msc_rs_corrected_total);
        adjusted.msc_rs_corrected_superframes = saturate_u32(
            worker->m_msc_rs_corrected_superframes_offset +
            snapshot->msc_rs_corrected_superframes);
        adjusted.msc_rs_uncorrectable_total = saturate_u32(
            worker->m_msc_rs_uncorrectable_offset +
            snapshot->msc_rs_uncorrectable_total);
        adjusted.msc_au_crc_errors_total = saturate_u32(
            worker->m_msc_au_crc_errors_offset + snapshot->msc_au_crc_errors_total);
        adjusted.msc_au_crc_checks_total = saturate_u32(
            worker->m_msc_au_crc_checks_offset + snapshot->msc_au_crc_checks_total);
        adjusted.msc_ber_error_bits += worker->m_msc_ber_error_bits_offset;
        adjusted.msc_ber_compared_bits += worker->m_msc_ber_compared_bits_offset;
        adjusted.msc_ber_valid = adjusted.msc_ber_compared_bits != 0u;
        adjusted.msc_ber = adjusted.msc_ber_valid
            ? static_cast<float>(
                static_cast<double>(adjusted.msc_ber_error_bits) /
                static_cast<double>(adjusted.msc_ber_compared_bits))
            : 0.0f;
        adjusted.msc_input_confidence_sum +=
            worker->m_msc_input_confidence_sum_offset;
        adjusted.msc_input_confidence_samples +=
            worker->m_msc_input_confidence_samples_offset;
        adjusted.msc_input_confidence_percent =
            adjusted.msc_input_confidence_samples != 0u
                ? static_cast<float>(
                    100.0 *
                    static_cast<double>(adjusted.msc_input_confidence_sum) /
                    (127.0 *
                     static_cast<double>(
                         adjusted.msc_input_confidence_samples)))
                : 0.0f;
        adjusted.msc_deinterleaver_cifs_seen +=
            worker->m_msc_deinterleaver_cifs_offset;
        adjusted.msc_cif_decode_attempts +=
            worker->m_msc_cif_attempts_offset;
        adjusted.msc_cif_decode_successes +=
            worker->m_msc_cif_successes_offset;
        adjusted.msc_cif_decode_failures +=
            worker->m_msc_cif_failures_offset;
        adjusted.msc_output_bytes += worker->m_msc_output_bytes_offset;
        adjusted.msc_cif_discontinuity_count = saturate_u32(
            worker->m_msc_cif_discontinuities_offset +
            snapshot->msc_cif_discontinuity_count);
        if (snapshot->hardware_overruns_valid != 0)
        {
            adjusted.hardware_overruns = saturate_u32(
                worker->m_hardware_overruns_offset + snapshot->hardware_overruns);
        }
        for (uint32_t index = 0u;
             index < adjusted.msc_dabplus_history_count;
             ++index)
        {
            adjusted.msc_dabplus_history[index].attempt_number =
                saturate_u32(
                    worker->m_msc_decode_attempts_offset +
                    adjusted.msc_dabplus_history[index].attempt_number);
            adjusted.msc_dabplus_history[index].generation +=
                worker->m_msc_generation_offset;
        }
        worker->ApplyAudioMeasurements(adjusted);
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
        m_audio_output_active.store(false, std::memory_order_release);
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
            std::scoped_lock measurement_lock{ m_audio_measurement_mutex };
            m_audio_measurements.format_valid = true;
            m_audio_measurements.sample_rate = event.sample_rate;
            m_audio_measurements.channels = event.channels;
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
        command.generation = event.generation + m_msc_generation_offset;
        command.audio_unit_index = event.audio_unit_index;
        command.audio_unit_start = event.audio_unit_start;
        command.crc_valid = event.audio_crc_valid != 0u;
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
                {
                    std::scoped_lock measurement_lock{
                        m_audio_measurement_mutex
                    };
                    m_audio_measurements.queue_drops++;
                }
            }
            m_audio_commands.push_back(std::move(command));
        }
        m_audio_queue_cv.notify_one();
    }

    void ReceiverWorker::ResetAudioMeasurements() noexcept
    {
        try
        {
            std::scoped_lock lock{ m_audio_measurement_mutex };
            m_audio_measurements = {};
        }
        catch (...)
        {
        }
    }

    void ReceiverWorker::CaptureAudioPlaybackDiagnostics(
        bool reset_baseline) noexcept
    {
        dab_audio_playback_diagnostics_t diagnostics{};
        if (dab_audio_playback_get_diagnostics(
                &m_audio_playback,
                &diagnostics) != 0)
        {
            return;
        }
        try
        {
            std::scoped_lock lock{ m_audio_measurement_mutex };
            auto& state = m_audio_measurements;
            auto add_delta = [](uint64_t current,
                                uint64_t& previous,
                                uint64_t& total)
            {
                total += current >= previous
                    ? current - previous
                    : current;
                previous = current;
            };
            uint64_t unused_pcm_total = 0u;
            add_delta(diagnostics.pcm_frames_written,
                      state.last_pcm_frames,
                      unused_pcm_total);
            add_delta(diagnostics.output_frames,
                      state.last_output_frames,
                      state.output_frames);
            add_delta(diagnostics.muted_frames,
                      state.last_muted_frames,
                      state.muted_frames);
            add_delta(diagnostics.underrun_events,
                      state.last_underrun_events,
                      state.underrun_events);
            add_delta(diagnostics.clipped_samples,
                      state.last_clipped_samples,
                      state.clipped_samples);
            add_delta(diagnostics.total_samples,
                      state.last_total_samples,
                      state.total_samples);
            state.longest_interruption_frames = (std::max)(
                state.longest_interruption_frames,
                diagnostics.longest_interruption_frames);
            state.decoder_backend = diagnostics.decoder_backend;
            if (diagnostics.pcm_frames_written != 0u)
            {
                state.level_valid = true;
                state.left_rms_dbfs = diagnostics.left_rms_dbfs;
                state.right_rms_dbfs = diagnostics.right_rms_dbfs;
                state.left_peak_dbfs = diagnostics.left_peak_dbfs;
                state.right_peak_dbfs = diagnostics.right_peak_dbfs;
            }
            if (reset_baseline)
            {
                state.last_pcm_frames = 0u;
                state.last_output_frames = 0u;
                state.last_muted_frames = 0u;
                state.last_underrun_events = 0u;
                state.last_clipped_samples = 0u;
                state.last_total_samples = 0u;
            }
        }
        catch (...)
        {
        }
    }

    void ReceiverWorker::RecordAudioAccessUnit(
        AudioCommand const& command,
        dab_receive_audio_au_result_t result) noexcept
    {
        try
        {
            dab_receive_audio_au_history_entry_t entry{};
            entry.valid = 1u;
            entry.au_index = static_cast<uint8_t>(command.audio_unit_index);
            entry.crc_valid = command.crc_valid ? 1u : 0u;
            entry.decoder_result = static_cast<uint8_t>(result);
            entry.generation = command.generation;
            entry.timestamp_ns = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::system_clock::now().time_since_epoch())
                    .count());
            entry.start_offset = command.audio_unit_start;
            entry.length = static_cast<uint32_t>(command.data.size());
            entry.preview_length = static_cast<uint8_t>((std::min)(
                command.data.size(),
                static_cast<size_t>(
                    DAB_RECEIVE_AUDIO_AU_PREVIEW_CAPACITY)));
            if (entry.preview_length != 0u)
            {
                std::copy_n(command.data.begin(),
                            entry.preview_length,
                            entry.preview);
            }

            bool pad_present = false;
            bool pad_malformed = false;
            if (command.crc_valid &&
                command.data.size() >= 3u &&
                (command.data[0] >> 5u) == 4u)
            {
                size_t pad_start = 2u;
                size_t pad_length = command.data[1];
                pad_present = true;
                if (pad_length == 255u)
                {
                    if (command.data.size() < 4u)
                    {
                        pad_malformed = true;
                    }
                    else
                    {
                        pad_length += command.data[2];
                        pad_start++;
                    }
                }
                if (!pad_malformed &&
                    (pad_length < 2u ||
                     pad_start + pad_length > command.data.size()))
                {
                    pad_malformed = true;
                }
            }
            entry.pad_present = pad_present ? 1u : 0u;

            std::scoped_lock lock{ m_audio_measurement_mutex };
            auto& state = m_audio_measurements;
            state.au_received++;
            if (result == DAB_RECEIVE_AUDIO_AU_CRC_REJECTED)
            {
                state.au_crc_rejected++;
            }
            else if (result == DAB_RECEIVE_AUDIO_AU_DECODED)
            {
                state.au_decoded++;
            }
            else if (result == DAB_RECEIVE_AUDIO_AU_DECODE_ERROR)
            {
                state.aac_errors++;
            }
            else if (result == DAB_RECEIVE_AUDIO_AU_OUTPUT_ERROR)
            {
                state.output_errors++;
            }
            if (pad_present)
            {
                state.pad_access_units++;
            }
            if (pad_malformed)
            {
                state.pad_parse_errors++;
            }
            state.au_history.push_back(entry);
            while (state.au_history.size() >
                   DAB_RECEIVE_AUDIO_AU_HISTORY_CAPACITY)
            {
                state.au_history.pop_front();
            }
        }
        catch (...)
        {
        }
    }

    void ReceiverWorker::ApplyAudioMeasurements(
        dab_receive_snapshot_t& snapshot) noexcept
    {
        try
        {
            std::scoped_lock lock{ m_audio_measurement_mutex };
            auto const& state = m_audio_measurements;
            snapshot.audio_format_valid = state.format_valid ? 1 : 0;
            snapshot.audio_output_active =
                m_audio_output_active.load(std::memory_order_acquire) ? 1 : 0;
            snapshot.audio_decoder_backend = state.decoder_backend;
            snapshot.audio_sample_rate_hz = state.sample_rate;
            snapshot.audio_channels = state.channels;
            snapshot.audio_au_received = state.au_received;
            snapshot.audio_au_crc_rejected = state.au_crc_rejected;
            snapshot.audio_au_decoded = state.au_decoded;
            snapshot.audio_aac_errors = state.aac_errors;
            snapshot.audio_output_errors = state.output_errors;
            snapshot.audio_queue_drops = state.queue_drops;
            snapshot.audio_level_valid = state.level_valid ? 1 : 0;
            snapshot.audio_left_rms_dbfs = state.left_rms_dbfs;
            snapshot.audio_right_rms_dbfs = state.right_rms_dbfs;
            snapshot.audio_left_peak_dbfs = state.left_peak_dbfs;
            snapshot.audio_right_peak_dbfs = state.right_peak_dbfs;
            snapshot.audio_clipped_samples = state.clipped_samples;
            snapshot.audio_total_samples = state.total_samples;
            snapshot.audio_clipping_percent =
                state.total_samples != 0u
                    ? static_cast<float>(
                        100.0 *
                        static_cast<double>(state.clipped_samples) /
                        static_cast<double>(state.total_samples))
                    : 0.0f;
            snapshot.audio_output_frames = state.output_frames;
            snapshot.audio_muted_frames = state.muted_frames;
            snapshot.audio_underrun_events = state.underrun_events;
            snapshot.audio_longest_interruption_frames =
                state.longest_interruption_frames;
            snapshot.audio_availability_percent =
                state.output_frames != 0u
                    ? static_cast<float>(
                        100.0 *
                        static_cast<double>(
                            state.output_frames - (std::min)(
                                state.output_frames,
                                state.muted_frames)) /
                        static_cast<double>(state.output_frames))
                    : 0.0f;
            snapshot.pad_access_units = state.pad_access_units;
            snapshot.pad_parse_errors = state.pad_parse_errors;
            snapshot.pad_metadata_decoder_supported = 0u;
            snapshot.audio_au_history_count =
                static_cast<uint32_t>((std::min)(
                    state.au_history.size(),
                    static_cast<size_t>(
                        DAB_RECEIVE_AUDIO_AU_HISTORY_CAPACITY)));
            size_t const first =
                state.au_history.size() - snapshot.audio_au_history_count;
            for (uint32_t index = 0u;
                 index < snapshot.audio_au_history_count;
                 ++index)
            {
                snapshot.audio_au_history[index] =
                    state.au_history[first + index];
            }
        }
        catch (...)
        {
        }
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
            m_audio_output_active.store(false, std::memory_order_release);
            CaptureAudioPlaybackDiagnostics(true);
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
                    m_audio_output_active.store(false, std::memory_order_release);
                    CaptureAudioPlaybackDiagnostics(true);
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
                    m_audio_output_active.store(false, std::memory_order_release);
                    CaptureAudioPlaybackDiagnostics(true);
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
                    if (!command->crc_valid)
                    {
                        RecordAudioAccessUnit(
                            *command,
                            DAB_RECEIVE_AUDIO_AU_CRC_REJECTED);
                        continue;
                    }
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
                            RecordAudioAccessUnit(
                                *command,
                                DAB_RECEIVE_AUDIO_AU_OUTPUT_ERROR);
                            schedule_retry("Audio decoder or output setup failed");
                            continue;
                        }
                        decoder_configured = true;
                    }
                    if (!decoder_configured)
                    {
                        RecordAudioAccessUnit(
                            *command,
                            DAB_RECEIVE_AUDIO_AU_OUTPUT_ERROR);
                        continue;
                    }

                    int const playback_result = dab_audio_playback_consume_au(
                        &m_audio_playback,
                        command->data.data(),
                        static_cast<uint32_t>(command->data.size()),
                        command->generation);
                    if (playback_result < 0)
                    {
                        RecordAudioAccessUnit(
                            *command,
                            DAB_RECEIVE_AUDIO_AU_OUTPUT_ERROR);
                        schedule_retry("Audio decoding or output failed");
                        continue;
                    }
                    RecordAudioAccessUnit(
                        *command,
                        playback_result == 0
                            ? DAB_RECEIVE_AUDIO_AU_DECODED
                            : DAB_RECEIVE_AUDIO_AU_DECODE_ERROR);
                    if (playback_result == 0 &&
                        dab_audio_playback_output_active(&m_audio_playback) > 0)
                    {
                        m_audio_output_active.store(true, std::memory_order_release);
                        if (recovering)
                        {
                            recovering = false;
                            retry_attempt = 0u;
                            PublishStatus(State(), "Audio output recovered without restarting the receiver");
                        }
                    }
                }

                if (decoder_configured && m_audio_playback.stream != nullptr)
                {
                    int const active = dab_audio_playback_output_active(&m_audio_playback);
                    if (active <= 0)
                    {
                        m_audio_output_active.store(false, std::memory_order_release);
                        schedule_retry("Audio output device stopped");
                    }
                }
                CaptureAudioPlaybackDiagnostics(false);
            }
        }
        catch (...)
        {
            PublishStatus(State(), "Audio worker stopped after an internal error", true);
        }
        m_audio_output_active.store(false, std::memory_order_release);
        CaptureAudioPlaybackDiagnostics(true);
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
            m_fic_frames_offset += m_last_session_snapshot.fic_frames_seen;
            m_fic_valid_fibs_offset += m_last_session_snapshot.fic_total_valid_fibs;
            m_fic_figs_offset += m_last_session_snapshot.fic_total_figs;
            m_fic_fig_errors_offset += m_last_session_snapshot.fic_total_fig_parse_errors;
            m_fic_pre_viterbi_error_bits_offset +=
                m_last_session_snapshot.fic_pre_viterbi_error_bits;
            m_fic_pre_viterbi_compared_bits_offset +=
                m_last_session_snapshot.fic_pre_viterbi_compared_bits;
            for (uint32_t index = 0u;
                 index < m_last_session_snapshot.fic_fig_stat_count;
                 ++index)
            {
                auto const& stat = m_last_session_snapshot.fic_fig_stats[index];
                size_t const key =
                    (static_cast<size_t>(stat.type & 0x07u) << 5u) |
                    static_cast<size_t>(stat.extension & 0x1Fu);
                m_fic_fig_count_offsets[key] += stat.total_count;
                m_fic_fig_change_offsets[key] += stat.change_count;
                m_fic_fig_malformed_offsets[key] += stat.malformed_count;
            }
            m_msc_frames_offset += m_last_session_snapshot.msc_frames_seen;
            m_msc_generation_offset += m_last_session_snapshot.msc_superframe_generation;
            m_msc_decode_attempts_offset += m_last_session_snapshot.msc_decode_attempts;
            m_msc_decode_failures_offset += m_last_session_snapshot.msc_decode_failures;
            m_msc_firecode_checks_offset +=
                m_last_session_snapshot.msc_firecode_checks_total;
            m_msc_firecode_errors_offset +=
                m_last_session_snapshot.msc_firecode_errors_total;
            m_msc_rs_corrected_offset += m_last_session_snapshot.msc_rs_corrected_total;
            m_msc_rs_corrected_superframes_offset +=
                m_last_session_snapshot.msc_rs_corrected_superframes;
            m_msc_rs_uncorrectable_offset +=
                m_last_session_snapshot.msc_rs_uncorrectable_total;
            m_msc_au_crc_errors_offset +=
                m_last_session_snapshot.msc_au_crc_errors_total;
            m_msc_au_crc_checks_offset +=
                m_last_session_snapshot.msc_au_crc_checks_total;
            m_msc_ber_error_bits_offset += m_last_session_snapshot.msc_ber_error_bits;
            m_msc_ber_compared_bits_offset +=
                m_last_session_snapshot.msc_ber_compared_bits;
            m_msc_input_confidence_sum_offset +=
                m_last_session_snapshot.msc_input_confidence_sum;
            m_msc_input_confidence_samples_offset +=
                m_last_session_snapshot.msc_input_confidence_samples;
            m_msc_deinterleaver_cifs_offset +=
                m_last_session_snapshot.msc_deinterleaver_cifs_seen;
            m_msc_cif_attempts_offset +=
                m_last_session_snapshot.msc_cif_decode_attempts;
            m_msc_cif_successes_offset +=
                m_last_session_snapshot.msc_cif_decode_successes;
            m_msc_cif_failures_offset +=
                m_last_session_snapshot.msc_cif_decode_failures;
            m_msc_output_bytes_offset +=
                m_last_session_snapshot.msc_output_bytes;
            m_msc_cif_discontinuities_offset +=
                m_last_session_snapshot.msc_cif_discontinuity_count;
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
