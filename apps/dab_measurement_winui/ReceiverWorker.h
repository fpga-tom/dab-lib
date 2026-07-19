#pragma once

#include "MeasurementMailbox.h"
#include "SnapshotMailbox.h"

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
        stopping,
        failed,
    };

    struct ReceiverSettings
    {
        std::string driver{ "sdrplay" };
        uint64_t frequency_hz{ 220352000ull };
        uint32_t service_id{ 0x00001001u };
        int gain_db{ 40 };
        bool agc_enabled{ false };
        int ppm_correction{ 0 };
        bool bias_tee_enabled{ false };
        bool use_real_device{ true };
        uint32_t measurement_decimation{ 1 };
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
        ReceiverWorker(SnapshotMailbox& mailbox, MeasurementMailbox& measurement_mailbox) noexcept;
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

        int OnEvent(dab_receive_event_t const& event) noexcept;
        void Run(ReceiverSettings settings) noexcept;
        void PublishStatus(ReceiverState state,
                           std::string message,
                           bool is_error = false,
                           int result_code = 0) noexcept;

        SnapshotMailbox& m_mailbox;
        MeasurementMailbox& m_measurement_mailbox;
        std::atomic<bool> m_cancel_requested{ false };
        std::atomic<ReceiverState> m_state{ ReceiverState::stopped };
        std::thread m_thread;
        mutable std::mutex m_lifecycle_mutex;
        mutable std::mutex m_status_mutex;
        ReceiverStatus m_latest_status{};
        uint64_t m_status_version{ 0 };
    };
}
