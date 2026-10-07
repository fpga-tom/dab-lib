#pragma once

#include "CatalogMailbox.h"
#include "MeasurementSessionRecorder.h"
#include "MeasurementMailbox.h"
#include "SnapshotMailbox.h"
#include "audio_playback.h"

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace dab::measurement
{
    enum class ReceiverState
    {
        stopped,
        starting,
        running,
        recovering,
        stopping,
        failed,
    };

    struct ReceiverSettings
    {
        std::string driver{ "sdrplay" };
        uint64_t frequency_hz{ 220352000ull };
        uint32_t service_id{ 0u };
        int gain_db{ 40 };
        bool agc_enabled{ false };
        int ppm_correction{ 0 };
        bool bias_tee_enabled{ false };
        bool use_real_device{ true };
        uint32_t measurement_decimation{ 1 };
        uint32_t stream_watchdog_timeout_ms{ 1500 };
        uint32_t decoder_watchdog_timeout_ms{ 1500 };
    };

    struct ReceiverStatus
    {
        ReceiverState state{ ReceiverState::stopped };
        std::string message;
        int result_code{ 0 };
        bool is_error{ false };
    };

    class ReceiverWorker final
    {
    public:
        ReceiverWorker(SnapshotMailbox& mailbox,
                       MeasurementMailbox& measurement_mailbox,
                       CatalogMailbox& catalog_mailbox,
                       MeasurementSessionRecorder& session_recorder) noexcept;
        ~ReceiverWorker();

        ReceiverWorker(ReceiverWorker const&) = delete;
        ReceiverWorker& operator=(ReceiverWorker const&) = delete;

        bool Start(ReceiverSettings settings);
        void RequestStop() noexcept;
        void JoinFinished();
        void StopAndJoin() noexcept;

        [[nodiscard]] ReceiverState State() const noexcept;
        [[nodiscard]] bool IsActive() const noexcept;
        [[nodiscard]] bool AudioOutputActive() const noexcept;
        void SetCalibrationProfile(
            dab_rf_calibration_profile_t const& profile) noexcept;
        void ClearCalibrationProfile() noexcept;
        bool TryReadStatus(uint64_t after_version,
                           ReceiverStatus& status,
                           uint64_t& version) const;

    private:
        enum class AudioCommandKind
        {
            format,
            access_unit,
            reset,
        };

        struct AudioCommand
        {
            AudioCommandKind kind{ AudioCommandKind::reset };
            uint32_t service_id{ 0u };
            uint64_t frequency_hz{ 0u };
            uint32_t sample_rate{ 0u };
            uint32_t channels{ 0u };
            std::array<uint8_t, 8> asc{};
            uint32_t asc_length{ 0u };
            std::vector<uint8_t> data;
            uint64_t generation{ 0u };
            uint32_t audio_unit_index{ 0u };
            uint32_t audio_unit_start{ 0u };
            bool crc_valid{ false };
        };

        struct AudioMeasurementState
        {
            bool format_valid{ false };
            uint8_t decoder_backend{ DAB_AUDIO_DECODER_NONE };
            uint32_t sample_rate{ 0u };
            uint32_t channels{ 0u };
            uint64_t au_received{ 0u };
            uint64_t au_crc_rejected{ 0u };
            uint64_t au_decoded{ 0u };
            uint64_t aac_errors{ 0u };
            uint64_t output_errors{ 0u };
            uint64_t queue_drops{ 0u };
            bool level_valid{ false };
            float left_rms_dbfs{ 0.0f };
            float right_rms_dbfs{ 0.0f };
            float left_peak_dbfs{ 0.0f };
            float right_peak_dbfs{ 0.0f };
            uint64_t clipped_samples{ 0u };
            uint64_t total_samples{ 0u };
            uint64_t output_frames{ 0u };
            uint64_t muted_frames{ 0u };
            uint64_t underrun_events{ 0u };
            uint64_t longest_interruption_frames{ 0u };
            uint64_t last_pcm_frames{ 0u };
            uint64_t last_output_frames{ 0u };
            uint64_t last_muted_frames{ 0u };
            uint64_t last_underrun_events{ 0u };
            uint64_t last_clipped_samples{ 0u };
            uint64_t last_total_samples{ 0u };
            uint64_t pad_access_units{ 0u };
            uint64_t pad_parse_errors{ 0u };
            std::deque<dab_receive_audio_au_history_entry_t> au_history;
        };

        static int CancelCallback(void* user) noexcept;
        static int EventCallback(dab_receive_event_t const* event, void* user) noexcept;
        static int SnapshotCallback(dab_receive_snapshot_t const* snapshot, void* user) noexcept;
        static int MeasurementCallback(dab_receive_measurement_frame_t const* frame, void* user) noexcept;
        static int CatalogCallback(dab_receive_catalog_t const* catalog, void* user) noexcept;

        int OnEvent(dab_receive_event_t const& event) noexcept;
        void Run(ReceiverSettings settings) noexcept;
        void ResetPlaybackState() noexcept;
        bool StartAudioWorker();
        void StopAudioWorker() noexcept;
        void AudioThreadMain() noexcept;
        void EnqueueAudioFormat(dab_receive_event_t const& event);
        void EnqueueAudioAccessUnit(dab_receive_event_t const& event);
        void EnqueueAudioReset() noexcept;
        void ResetAudioMeasurements() noexcept;
        void CaptureAudioPlaybackDiagnostics(bool reset_baseline) noexcept;
        void RecordAudioAccessUnit(AudioCommand const& command,
                                   dab_receive_audio_au_result_t result) noexcept;
        void ApplyAudioMeasurements(dab_receive_snapshot_t& snapshot) noexcept;
        void ApplyCalibration(dab_receive_snapshot_t& snapshot) noexcept;
        void AccumulateRecoveredSession() noexcept;
        void PublishStatus(ReceiverState state,
                           std::string message,
                           bool is_error = false,
                           int result_code = 0) noexcept;

        SnapshotMailbox& m_mailbox;
        MeasurementMailbox& m_measurement_mailbox;
        CatalogMailbox& m_catalog_mailbox;
        MeasurementSessionRecorder& m_session_recorder;
        std::atomic<bool> m_cancel_requested{ false };
        std::atomic<ReceiverState> m_state{ ReceiverState::stopped };
        std::thread m_thread;
        mutable std::mutex m_lifecycle_mutex;
        mutable std::mutex m_status_mutex;
        ReceiverStatus m_latest_status{};
        uint64_t m_status_version{ 0 };
        bool m_audio_format_status_valid{ false };
        uint32_t m_audio_format_service_id{ 0u };
        uint64_t m_audio_format_frequency_hz{ 0u };
        uint32_t m_audio_format_sample_rate{ 0u };
        uint32_t m_audio_format_channels{ 0u };
        std::array<uint8_t, 8> m_audio_format_asc{};
        uint32_t m_audio_format_asc_length{ 0u };
        dab_audio_playback_t m_audio_playback{};
        std::thread m_audio_thread;
        std::mutex m_audio_queue_mutex;
        std::condition_variable m_audio_queue_cv;
        std::deque<AudioCommand> m_audio_commands;
        bool m_audio_stop_requested{ false };
        std::atomic<bool> m_audio_output_active{ false };
        std::mutex m_audio_measurement_mutex;
        AudioMeasurementState m_audio_measurements{};
        std::mutex m_calibration_mutex;
        dab_rf_calibration_profile_t m_calibration_profile{};
        bool m_calibration_profile_valid{ false };
        uint32_t m_recovery_attempt_count{ 0u };
        uint64_t m_snapshot_sequence_offset{ 0u };
        uint64_t m_measurement_sequence_offset{ 0u };
        uint64_t m_blocks_received_offset{ 0u };
        uint64_t m_dropped_blocks_offset{ 0u };
        uint64_t m_processing_late_offset{ 0u };
        uint64_t m_hardware_overruns_offset{ 0u };
        uint64_t m_fic_frames_offset{ 0u };
        uint64_t m_fic_valid_fibs_offset{ 0u };
        uint64_t m_fic_figs_offset{ 0u };
        uint64_t m_fic_fig_errors_offset{ 0u };
        uint64_t m_fic_pre_viterbi_error_bits_offset{ 0u };
        uint64_t m_fic_pre_viterbi_compared_bits_offset{ 0u };
        std::array<uint64_t, 256u> m_fic_fig_count_offsets{};
        std::array<uint64_t, 256u> m_fic_fig_change_offsets{};
        std::array<uint64_t, 256u> m_fic_fig_malformed_offsets{};
        uint64_t m_msc_frames_offset{ 0u };
        uint64_t m_msc_generation_offset{ 0u };
        uint64_t m_msc_decode_attempts_offset{ 0u };
        uint64_t m_msc_decode_failures_offset{ 0u };
        uint64_t m_msc_firecode_checks_offset{ 0u };
        uint64_t m_msc_firecode_errors_offset{ 0u };
        uint64_t m_msc_rs_corrected_offset{ 0u };
        uint64_t m_msc_rs_corrected_superframes_offset{ 0u };
        uint64_t m_msc_rs_uncorrectable_offset{ 0u };
        uint64_t m_msc_au_crc_errors_offset{ 0u };
        uint64_t m_msc_au_crc_checks_offset{ 0u };
        uint64_t m_msc_ber_error_bits_offset{ 0u };
        uint64_t m_msc_ber_compared_bits_offset{ 0u };
        uint64_t m_msc_input_confidence_sum_offset{ 0u };
        uint64_t m_msc_input_confidence_samples_offset{ 0u };
        uint64_t m_msc_deinterleaver_cifs_offset{ 0u };
        uint64_t m_msc_cif_attempts_offset{ 0u };
        uint64_t m_msc_cif_successes_offset{ 0u };
        uint64_t m_msc_cif_failures_offset{ 0u };
        uint64_t m_msc_output_bytes_offset{ 0u };
        uint64_t m_msc_cif_discontinuities_offset{ 0u };
        dab_receive_snapshot_t m_last_session_snapshot{};
        uint64_t m_last_session_measurement_sequence{ 0u };
        bool m_last_session_snapshot_valid{ false };
    };
}
