#pragma once

#include "MeasurementMailbox.h"
#include "MeasurementSessionRecorder.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include <vector>

namespace dab::measurement
{
    struct MeasurementSessionReplayInfo
    {
        std::filesystem::path session_path;
        std::string state;
        std::string context;
        std::string device;
        std::string driver;
        std::string serial;
        std::string channel;
        std::string ensemble;
        std::string service;
        uint64_t frequency_hz{ 0u };
        uint32_t sample_rate_hz{ 0u };
        uint64_t started_unix_ns{ 0u };
        uint64_t duration_ns{ 0u };
        uint64_t bytes_written{ 0u };
        uint64_t dropped_records{ 0u };
        uint64_t record_count{ 0u };
        uint64_t snapshot_records{ 0u };
        uint64_t measurement_records{ 0u };
        uint64_t catalog_records{ 0u };
        uint64_t event_records{ 0u };
        uint32_t chunk_count{ 0u };
    };

    struct MeasurementSessionTimelinePoint
    {
        uint64_t timestamp_ns{ 0u };
        float power{ 0.0f };
        float mer_db{ 0.0f };
        float cn_db{ 0.0f };
        float cfo_hz{ 0.0f };
        float msc_ber{ 0.0f };
        float clipping_percent{ 0.0f };
        uint32_t dropped_blocks{ 0u };
        bool power_valid{ false };
        bool power_calibrated{ false };
        bool mer_valid{ false };
        bool cn_valid{ false };
        bool cfo_valid{ false };
        bool msc_ber_valid{ false };
        bool clipping_valid{ false };
        bool locked{ false };
    };

    struct MeasurementSessionReplayEvent
    {
        uint64_t timestamp_ns{ 0u };
        uint32_t event_type{ 0u };
        std::string message;
    };

    struct MeasurementSessionReplayRecord
    {
        MeasurementSessionRecordType type{
            MeasurementSessionRecordType::metadata
        };
        uint64_t timestamp_ns{ 0u };
        uint64_t sequence{ 0u };
        std::vector<uint8_t> payload;
    };

    class MeasurementSessionReplay final
    {
    public:
        bool Open(std::filesystem::path const& selected_path,
                  std::string& error);
        void Close() noexcept;

        [[nodiscard]] bool IsOpen() const noexcept;
        [[nodiscard]] MeasurementSessionReplayInfo const& Info() const noexcept;

        static bool ReadSessionInfo(
            std::filesystem::path const& selected_path,
            MeasurementSessionReplayInfo& info,
            std::string& error);
        bool BuildTimeline(
            size_t maximum_points,
            std::vector<MeasurementSessionTimelinePoint>& timeline,
            std::vector<MeasurementSessionReplayEvent>& events,
            std::string& error) const;

        bool Seek(uint64_t timestamp_ns,
                  std::vector<MeasurementSessionReplayRecord>& state,
                  std::string& error);
        bool ReadUntil(uint64_t timestamp_ns,
                       std::vector<MeasurementSessionReplayRecord>& records,
                       size_t maximum_records,
                       std::string& error);

        static bool DecodeSnapshot(
            MeasurementSessionReplayRecord const& record,
            dab_receive_snapshot_t& snapshot,
            std::string& error);
        static bool DecodeMeasurement(
            MeasurementSessionReplayRecord const& record,
            MeasurementFrame& frame,
            std::string& error);
        static bool DecodeCatalog(
            MeasurementSessionReplayRecord const& record,
            dab_receive_catalog_t& catalog,
            std::string& error);
        static bool DecodeEvent(
            MeasurementSessionReplayRecord const& record,
            uint32_t& event_type,
            std::string& message,
            std::string& error);

    private:
        struct IndexEntry
        {
            MeasurementSessionRecordType type;
            uint64_t raw_timestamp_ns;
            uint64_t timestamp_ns;
            uint64_t sequence;
            uint32_t payload_size;
            uint32_t payload_crc32;
            size_t chunk;
            uint64_t payload_offset;
        };

        bool ReadRecord(size_t index,
                        MeasurementSessionReplayRecord& record,
                        std::string& error) const;

        MeasurementSessionReplayInfo m_info;
        std::vector<std::filesystem::path> m_chunks;
        std::vector<IndexEntry> m_index;
        mutable std::ifstream m_read_stream;
        mutable size_t m_read_chunk{
            (std::numeric_limits<size_t>::max)()
        };
        size_t m_cursor{ 0u };
        bool m_open{ false };
    };
}
