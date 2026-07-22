#pragma once

#include "CatalogMailbox.h"
#include "MeasurementMailbox.h"
#include "SnapshotMailbox.h"
#include "audio_playback.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>

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
                       CatalogMailbox& catalog_mailbox) noexcept;
        ~ReceiverWorker();

        ReceiverWorker(ReceiverWorker const&) = delete;
        ReceiverWorker& operator=(ReceiverWorker const&) = delete;

        bool Start(ReceiverSettings settings);
        void RequestStop() noexcept;
        void JoinFinished();
        void StopAndJoin() noexcept;

        [[nodiscard]] ReceiverState State() const noexcept;
        [[nodiscard]] bool IsActive() const noexcept;
        bool TryReadStatus(uint64_t after_version,
                           ReceiverStatus& status,
                           uint64_t& version) const;

    private:
        static int CancelCallback(void* user) noexcept;
        static int EventCallback(dab_receive_event_t const* event, void* user) noexcept;
        static int SnapshotCallback(dab_receive_snapshot_t const* snapshot, void* user) noexcept;
        static int MeasurementCallback(dab_receive_measurement_frame_t const* frame, void* user) noexcept;

        int OnEvent(dab_receive_event_t const& event) noexcept;
        void Run(ReceiverSettings settings) noexcept;
        void ResetPlaybackState() noexcept;
        void AccumulateRecoveredSession() noexcept;
        void PublishStatus(ReceiverState state,
                           std::string message,
                           bool is_error = false,
                           int result_code = 0) noexcept;

        SnapshotMailbox& m_mailbox;
        MeasurementMailbox& m_measurement_mailbox;
        CatalogMailbox& m_catalog_mailbox;
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
        bool m_audio_playback_failed{ false };
        uint32_t m_recovery_attempt_count{ 0u };
        uint64_t m_snapshot_sequence_offset{ 0u };
        uint64_t m_measurement_sequence_offset{ 0u };
        uint64_t m_blocks_received_offset{ 0u };
        uint64_t m_dropped_blocks_offset{ 0u };
        uint64_t m_processing_late_offset{ 0u };
        uint64_t m_hardware_overruns_offset{ 0u };
        dab_receive_snapshot_t m_last_session_snapshot{};
        uint64_t m_last_session_measurement_sequence{ 0u };
        bool m_last_session_snapshot_valid{ false };
    };
}
