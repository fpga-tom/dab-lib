#pragma once

#include "dab/receive.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace dab::measurement
{
    inline constexpr uint32_t measurement_session_format_version = 1u;
    inline constexpr uint32_t measurement_session_record_magic = 0x43455244u;

    enum class MeasurementSessionRecordType : uint16_t
    {
        metadata = 1,
        snapshot = 2,
        measurement = 3,
        catalog = 4,
        event = 5,
    };

#pragma pack(push, 1)
    struct MeasurementSessionChunkHeader
    {
        char magic[8];
        uint32_t format_version;
        uint32_t chunk_index;
        uint64_t session_start_unix_ns;
        uint64_t reserved;
    };

    struct MeasurementSessionRecordHeader
    {
        uint32_t magic;
        uint16_t header_version;
        uint16_t record_type;
        uint64_t timestamp_ns;
        uint64_t sequence;
        uint32_t payload_size;
        uint32_t payload_crc32;
    };

    struct MeasurementSessionFrameHeader
    {
        uint64_t frequency_hz;
        uint64_t frame_start_sample;
        uint32_t sample_rate_hz;
        uint32_t spectrum_bin_count;
        float spectrum_start_hz;
        float spectrum_bin_hz;
        uint32_t constellation_point_count;
        uint32_t cir_bin_count;
        float cir_sample_period_ns;
        uint32_t channel_response_count;
        uint32_t carrier_mer_count;
        int32_t first_carrier;
        uint32_t negative_carrier_count;
        float carrier_spacing_hz;
    };
#pragma pack(pop)

    static_assert(sizeof(MeasurementSessionChunkHeader) == 32u);
    static_assert(sizeof(MeasurementSessionRecordHeader) == 32u);

    struct MeasurementSessionStartInfo
    {
        std::filesystem::path recordings_root;
        std::string application_version{ "0.1.3" };
        std::string context;
        std::string device;
        std::string driver;
        std::string serial;
        std::string channel;
        std::string ensemble;
        std::string service;
        std::string calibration_profile_id;
        uint64_t frequency_hz{ 0u };
        uint32_t sample_rate_hz{ 0u };
        int gain_reduction_db{ 0 };
        bool agc_enabled{ false };
    };

    struct MeasurementSessionRecorderLimits
    {
        size_t maximum_queue_bytes{ 64u * 1024u * 1024u };
        uint64_t maximum_chunk_bytes{ 256u * 1024u * 1024u };
    };

    struct MeasurementSessionRecorderStatus
    {
        bool active{ false };
        bool failed{ false };
        uint64_t elapsed_ns{ 0u };
        uint64_t bytes_written{ 0u };
        uint64_t queued_bytes{ 0u };
        uint64_t dropped_records{ 0u };
        uint64_t snapshot_records{ 0u };
        uint64_t measurement_records{ 0u };
        uint64_t catalog_records{ 0u };
        uint64_t event_records{ 0u };
        uint32_t chunk_count{ 0u };
        std::filesystem::path session_path;
        std::string error;
    };

    class MeasurementSessionRecorder final
    {
    public:
        explicit MeasurementSessionRecorder(
            MeasurementSessionRecorderLimits limits = {});
        ~MeasurementSessionRecorder();

        MeasurementSessionRecorder(MeasurementSessionRecorder const&) = delete;
        MeasurementSessionRecorder& operator=(
            MeasurementSessionRecorder const&) = delete;

        bool Start(MeasurementSessionStartInfo info,
                   std::string& error) noexcept;
        MeasurementSessionRecorderStatus Stop() noexcept;

        void RecordSnapshot(dab_receive_snapshot_t const& snapshot) noexcept;
        void RecordMeasurement(
            dab_receive_measurement_frame_t const& frame) noexcept;
        void RecordCatalog(dab_receive_catalog_t const& catalog) noexcept;
        void RecordEvent(uint32_t event_type,
                         std::string const& message,
                         uint64_t timestamp_ns = 0u,
                         uint64_t sequence = 0u) noexcept;

        [[nodiscard]] bool IsActive() const noexcept;
        [[nodiscard]] MeasurementSessionRecorderStatus Status() const;
        [[nodiscard]] static uint32_t Crc32(
            void const* data,
            size_t size) noexcept;

    private:
        struct PendingRecord
        {
            MeasurementSessionRecordType type;
            uint64_t timestamp_ns;
            uint64_t sequence;
            std::vector<uint8_t> payload;
        };

        void Enqueue(PendingRecord record) noexcept;
        void WriterLoop() noexcept;
        bool OpenChunk(uint32_t index, std::string& error);
        void Fail(std::string message) noexcept;
        void WriteManifest(char const* state) noexcept;
        uint64_t ElapsedNs() const noexcept;
        std::string MetadataJson() const;

        MeasurementSessionRecorderLimits m_limits;
        mutable std::mutex m_mutex;
        std::condition_variable m_condition;
        std::deque<PendingRecord> m_queue;
        size_t m_queued_bytes{ 0u };
        bool m_stop_requested{ false };
        std::thread m_writer;
        std::ofstream m_output;
        MeasurementSessionStartInfo m_info;
        std::filesystem::path m_session_path;
        std::string m_error;
        std::chrono::steady_clock::time_point m_started_steady{};
        uint64_t m_started_unix_ns{ 0u };
        uint64_t m_current_chunk_bytes{ 0u };
        uint32_t m_current_chunk_index{ 0u };

        std::atomic<bool> m_active{ false };
        std::atomic<bool> m_failed{ false };
        std::atomic<uint64_t> m_bytes_written{ 0u };
        std::atomic<uint64_t> m_final_elapsed_ns{ 0u };
        std::atomic<uint64_t> m_dropped_records{ 0u };
        std::atomic<uint64_t> m_snapshot_records{ 0u };
        std::atomic<uint64_t> m_measurement_records{ 0u };
        std::atomic<uint64_t> m_catalog_records{ 0u };
        std::atomic<uint64_t> m_event_records{ 0u };
        std::atomic<uint32_t> m_chunk_count{ 0u };
    };
}
