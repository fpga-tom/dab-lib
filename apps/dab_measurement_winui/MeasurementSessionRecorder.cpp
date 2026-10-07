#include "MeasurementSessionRecorder.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <cstring>
#include <iomanip>
#include <sstream>
#include <string_view>
#include <system_error>
#include <utility>

namespace
{
    template <typename Value>
    void append_value(std::vector<uint8_t>& destination,
                      Value const& value)
    {
        auto const* bytes = reinterpret_cast<uint8_t const*>(&value);
        destination.insert(destination.end(), bytes, bytes + sizeof(value));
    }

    template <typename Value>
    void append_values(std::vector<uint8_t>& destination,
                       Value const* values,
                       size_t count)
    {
        if (values == nullptr || count == 0u)
        {
            return;
        }
        auto const* bytes = reinterpret_cast<uint8_t const*>(values);
        destination.insert(destination.end(),
                           bytes,
                           bytes + sizeof(Value) * count);
    }

    std::string json_escape(std::string_view value)
    {
        std::string escaped;
        escaped.reserve(value.size() + 8u);
        for (unsigned char character : value)
        {
            switch (character)
            {
            case '"':
                escaped += "\\\"";
                break;
            case '\\':
                escaped += "\\\\";
                break;
            case '\b':
                escaped += "\\b";
                break;
            case '\f':
                escaped += "\\f";
                break;
            case '\n':
                escaped += "\\n";
                break;
            case '\r':
                escaped += "\\r";
                break;
            case '\t':
                escaped += "\\t";
                break;
            default:
                if (character < 0x20u)
                {
                    char buffer[8]{};
                    std::snprintf(buffer,
                                  sizeof(buffer),
                                  "\\u%04x",
                                  static_cast<unsigned int>(character));
                    escaped += buffer;
                }
                else
                {
                    escaped.push_back(static_cast<char>(character));
                }
                break;
            }
        }
        return escaped;
    }

    std::string local_session_name()
    {
        std::time_t const now = std::time(nullptr);
        std::tm local{};
#ifdef _WIN32
        localtime_s(&local, &now);
#else
        localtime_r(&now, &local);
#endif
        std::ostringstream name;
        name << "DAB_" << std::put_time(&local, "%Y%m%d_%H%M%S");
        return name.str();
    }

    std::string chunk_filename(uint32_t index)
    {
        std::ostringstream name;
        name << "data-" << std::setfill('0') << std::setw(4) << index
             << ".dabm";
        return name.str();
    }

    std::string record_event_payload(uint32_t event_type,
                                     std::string const& message)
    {
        std::ostringstream payload;
        payload << "{\"event_type\":" << event_type
                << ",\"message\":\"" << json_escape(message) << "\"}";
        return payload.str();
    }
}

namespace dab::measurement
{
    MeasurementSessionRecorder::MeasurementSessionRecorder(
        MeasurementSessionRecorderLimits limits)
        : m_limits(limits)
    {
        if (m_limits.maximum_queue_bytes < 1024u)
        {
            m_limits.maximum_queue_bytes = 1024u;
        }
        if (m_limits.maximum_chunk_bytes < 1024u * 1024u)
        {
            m_limits.maximum_chunk_bytes = 1024u * 1024u;
        }
    }

    MeasurementSessionRecorder::~MeasurementSessionRecorder()
    {
        Stop();
    }

    bool MeasurementSessionRecorder::Start(MeasurementSessionStartInfo info,
                                           std::string& error) noexcept
    {
        try
        {
            Stop();
            if (info.recordings_root.empty())
            {
                error = "Recording folder is empty";
                return false;
            }

            std::error_code filesystem_error;
            std::filesystem::create_directories(info.recordings_root,
                                                filesystem_error);
            if (filesystem_error)
            {
                error = "Could not create the recordings folder: " +
                    filesystem_error.message();
                return false;
            }

            std::filesystem::path session =
                info.recordings_root / local_session_name();
            for (uint32_t suffix = 2u;
                 std::filesystem::exists(session, filesystem_error);
                 ++suffix)
            {
                session = info.recordings_root /
                    (local_session_name() + "-" + std::to_string(suffix));
            }
            std::filesystem::create_directories(session, filesystem_error);
            if (filesystem_error)
            {
                error = "Could not create the measurement session: " +
                    filesystem_error.message();
                return false;
            }

            m_info = std::move(info);
            m_session_path = std::move(session);
            m_started_steady = std::chrono::steady_clock::now();
            m_started_unix_ns = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count());
            m_error.clear();
            m_failed.store(false, std::memory_order_release);
            m_bytes_written.store(0u, std::memory_order_release);
            m_final_elapsed_ns.store(0u, std::memory_order_release);
            m_dropped_records.store(0u, std::memory_order_release);
            m_snapshot_records.store(0u, std::memory_order_release);
            m_measurement_records.store(0u, std::memory_order_release);
            m_catalog_records.store(0u, std::memory_order_release);
            m_event_records.store(0u, std::memory_order_release);
            m_chunk_count.store(0u, std::memory_order_release);
            {
                std::scoped_lock lock{ m_mutex };
                m_queue.clear();
                m_queued_bytes = 0u;
                m_stop_requested = false;
            }

            if (!OpenChunk(1u, error))
            {
                m_failed.store(true, std::memory_order_release);
                m_error = error;
                WriteManifest("failed");
                return false;
            }

            WriteManifest("recording");
            m_active.store(true, std::memory_order_release);
            m_writer = std::thread(&MeasurementSessionRecorder::WriterLoop,
                                   this);

            std::string metadata = MetadataJson();
            PendingRecord record{
                MeasurementSessionRecordType::metadata,
                0u,
                0u,
                std::vector<uint8_t>(metadata.begin(), metadata.end())
            };
            Enqueue(std::move(record));
            return true;
        }
        catch (std::exception const& exception)
        {
            error = std::string{ "Could not start recording: " } +
                exception.what();
        }
        catch (...)
        {
            error = "Could not start recording";
        }
        m_active.store(false, std::memory_order_release);
        m_failed.store(true, std::memory_order_release);
        return false;
    }

    MeasurementSessionRecorderStatus MeasurementSessionRecorder::Stop() noexcept
    {
        bool const was_active =
            m_active.exchange(false, std::memory_order_acq_rel);
        if (was_active ||
            (m_failed.load(std::memory_order_acquire) &&
             m_final_elapsed_ns.load(std::memory_order_acquire) == 0u))
        {
            m_final_elapsed_ns.store(ElapsedNs(),
                                     std::memory_order_release);
        }
        {
            std::scoped_lock lock{ m_mutex };
            m_stop_requested = true;
        }
        m_condition.notify_all();
        if (m_writer.joinable())
        {
            m_writer.join();
        }
        if (m_output.is_open())
        {
            m_output.flush();
            m_output.close();
        }
        if (!m_session_path.empty())
        {
            WriteManifest(m_failed.load(std::memory_order_acquire)
                ? "failed"
                : "complete");
        }
        return Status();
    }

    void MeasurementSessionRecorder::RecordSnapshot(
        dab_receive_snapshot_t const& snapshot) noexcept
    {
        if (!IsActive())
        {
            return;
        }
        try
        {
            PendingRecord record;
            record.type = MeasurementSessionRecordType::snapshot;
            record.timestamp_ns = ElapsedNs();
            record.sequence = snapshot.sequence;
            record.payload.resize(sizeof(snapshot));
            std::memcpy(record.payload.data(), &snapshot, sizeof(snapshot));
            Enqueue(std::move(record));
        }
        catch (...)
        {
            m_dropped_records.fetch_add(1u, std::memory_order_relaxed);
        }
    }

    void MeasurementSessionRecorder::RecordMeasurement(
        dab_receive_measurement_frame_t const& frame) noexcept
    {
        if (!IsActive())
        {
            return;
        }
        try
        {
            uint32_t const spectrum_count = frame.spectrum_dbfs != nullptr
                ? (std::min)(frame.spectrum_bin_count,
                             static_cast<uint32_t>(
                                 DAB_RECEIVE_SPECTRUM_BIN_COUNT))
                : 0u;
            uint32_t const constellation_count =
                frame.constellation_i != nullptr &&
                frame.constellation_q != nullptr
                    ? (std::min)(frame.constellation_point_count,
                                 static_cast<uint32_t>(
                                     DAB_RECEIVE_CONSTELLATION_POINT_CAPACITY))
                    : 0u;
            uint32_t const cir_count = frame.cir_power_db != nullptr
                ? (std::min)(frame.cir_bin_count,
                             static_cast<uint32_t>(
                                 DAB_RECEIVE_CIR_BIN_COUNT))
                : 0u;
            uint32_t const response_count =
                frame.channel_magnitude_db != nullptr &&
                frame.channel_phase_deg != nullptr
                    ? (std::min)(frame.channel_response_count,
                                 static_cast<uint32_t>(
                                     DAB_RECEIVE_CHANNEL_RESPONSE_COUNT))
                    : 0u;
            uint32_t const mer_count = frame.carrier_mer_db != nullptr
                ? (std::min)(frame.carrier_mer_count,
                             static_cast<uint32_t>(
                                 DAB_RECEIVE_CARRIER_MER_COUNT))
                : 0u;

            MeasurementSessionFrameHeader header{};
            header.frequency_hz = frame.frequency_hz;
            header.frame_start_sample = frame.frame_start_sample;
            header.sample_rate_hz = frame.sample_rate_hz;
            header.spectrum_bin_count = spectrum_count;
            header.spectrum_start_hz = frame.spectrum_start_hz;
            header.spectrum_bin_hz = frame.spectrum_bin_hz;
            header.constellation_point_count = constellation_count;
            header.cir_bin_count = cir_count;
            header.cir_sample_period_ns = frame.cir_sample_period_ns;
            header.channel_response_count = response_count;
            header.carrier_mer_count = mer_count;
            header.first_carrier = frame.first_carrier;
            header.negative_carrier_count = frame.negative_carrier_count;
            header.carrier_spacing_hz = frame.carrier_spacing_hz;

            PendingRecord record;
            record.type = MeasurementSessionRecordType::measurement;
            record.timestamp_ns = ElapsedNs();
            record.sequence = frame.sequence;
            size_t const float_count =
                spectrum_count +
                2u * static_cast<size_t>(constellation_count) +
                cir_count +
                2u * static_cast<size_t>(response_count) +
                mer_count;
            record.payload.reserve(sizeof(header) +
                                   float_count * sizeof(float));
            append_value(record.payload, header);
            append_values(record.payload,
                          frame.spectrum_dbfs,
                          spectrum_count);
            append_values(record.payload,
                          frame.constellation_i,
                          constellation_count);
            append_values(record.payload,
                          frame.constellation_q,
                          constellation_count);
            append_values(record.payload,
                          frame.cir_power_db,
                          cir_count);
            append_values(record.payload,
                          frame.channel_magnitude_db,
                          response_count);
            append_values(record.payload,
                          frame.channel_phase_deg,
                          response_count);
            append_values(record.payload,
                          frame.carrier_mer_db,
                          mer_count);
            Enqueue(std::move(record));
        }
        catch (...)
        {
            m_dropped_records.fetch_add(1u, std::memory_order_relaxed);
        }
    }

    void MeasurementSessionRecorder::RecordCatalog(
        dab_receive_catalog_t const& catalog) noexcept
    {
        if (!IsActive())
        {
            return;
        }
        try
        {
            PendingRecord record;
            record.type = MeasurementSessionRecordType::catalog;
            record.timestamp_ns = ElapsedNs();
            record.sequence = catalog.sequence;
            record.payload.resize(sizeof(catalog));
            std::memcpy(record.payload.data(), &catalog, sizeof(catalog));
            Enqueue(std::move(record));
        }
        catch (...)
        {
            m_dropped_records.fetch_add(1u, std::memory_order_relaxed);
        }
    }

    void MeasurementSessionRecorder::RecordEvent(
        uint32_t event_type,
        std::string const& message,
        uint64_t timestamp_ns,
        uint64_t sequence) noexcept
    {
        if (!IsActive())
        {
            return;
        }
        try
        {
            std::string payload = record_event_payload(event_type, message);
            PendingRecord record{
                MeasurementSessionRecordType::event,
                timestamp_ns != 0u ? timestamp_ns : ElapsedNs(),
                sequence,
                std::vector<uint8_t>(payload.begin(), payload.end())
            };
            Enqueue(std::move(record));
        }
        catch (...)
        {
            m_dropped_records.fetch_add(1u, std::memory_order_relaxed);
        }
    }

    bool MeasurementSessionRecorder::IsActive() const noexcept
    {
        return m_active.load(std::memory_order_acquire);
    }

    MeasurementSessionRecorderStatus
    MeasurementSessionRecorder::Status() const
    {
        MeasurementSessionRecorderStatus status;
        status.active = IsActive();
        status.failed = m_failed.load(std::memory_order_acquire);
        status.elapsed_ns = status.active
            ? ElapsedNs()
            : m_final_elapsed_ns.load(std::memory_order_acquire);
        status.bytes_written =
            m_bytes_written.load(std::memory_order_acquire);
        status.dropped_records =
            m_dropped_records.load(std::memory_order_acquire);
        status.snapshot_records =
            m_snapshot_records.load(std::memory_order_acquire);
        status.measurement_records =
            m_measurement_records.load(std::memory_order_acquire);
        status.catalog_records =
            m_catalog_records.load(std::memory_order_acquire);
        status.event_records =
            m_event_records.load(std::memory_order_acquire);
        status.chunk_count = m_chunk_count.load(std::memory_order_acquire);
        {
            std::scoped_lock lock{ m_mutex };
            status.queued_bytes = m_queued_bytes;
            status.session_path = m_session_path;
            status.error = m_error;
        }
        return status;
    }

    uint32_t MeasurementSessionRecorder::Crc32(
        void const* data,
        size_t size) noexcept
    {
        auto const* bytes = static_cast<uint8_t const*>(data);
        uint32_t crc = 0xffffffffu;
        for (size_t index = 0u; index < size; ++index)
        {
            crc ^= bytes[index];
            for (uint32_t bit = 0u; bit < 8u; ++bit)
            {
                uint32_t const mask =
                    static_cast<uint32_t>(-
                        static_cast<int32_t>(crc & 1u));
                crc = (crc >> 1u) ^ (0xedb88320u & mask);
            }
        }
        return ~crc;
    }

    void MeasurementSessionRecorder::Enqueue(PendingRecord record) noexcept
    {
        if (!IsActive())
        {
            return;
        }
        size_t const bytes =
            sizeof(MeasurementSessionRecordHeader) + record.payload.size();
        std::unique_lock lock{ m_mutex };
        if (!IsActive() || bytes > m_limits.maximum_queue_bytes ||
            m_queued_bytes > m_limits.maximum_queue_bytes - bytes)
        {
            m_dropped_records.fetch_add(1u, std::memory_order_relaxed);
            return;
        }
        m_queued_bytes += bytes;
        m_queue.push_back(std::move(record));
        lock.unlock();
        m_condition.notify_one();
    }

    void MeasurementSessionRecorder::WriterLoop() noexcept
    {
        for (;;)
        {
            PendingRecord record;
            {
                std::unique_lock lock{ m_mutex };
                m_condition.wait(lock, [this]
                {
                    return m_stop_requested || !m_queue.empty();
                });
                if (m_queue.empty())
                {
                    if (m_stop_requested)
                    {
                        break;
                    }
                    continue;
                }
                record = std::move(m_queue.front());
                m_queued_bytes -=
                    sizeof(MeasurementSessionRecordHeader) +
                    record.payload.size();
                m_queue.pop_front();
            }

            MeasurementSessionRecordHeader header{};
            header.magic = measurement_session_record_magic;
            header.header_version = 1u;
            header.record_type = static_cast<uint16_t>(record.type);
            header.timestamp_ns = record.timestamp_ns;
            header.sequence = record.sequence;
            header.payload_size =
                static_cast<uint32_t>(record.payload.size());
            header.payload_crc32 = Crc32(record.payload.data(),
                                         record.payload.size());
            uint64_t const record_bytes =
                sizeof(header) + record.payload.size();
            if (m_current_chunk_bytes > sizeof(MeasurementSessionChunkHeader) &&
                m_current_chunk_bytes + record_bytes >
                    m_limits.maximum_chunk_bytes)
            {
                m_output.flush();
                m_output.close();
                std::string error;
                if (!OpenChunk(m_current_chunk_index + 1u, error))
                {
                    Fail(std::move(error));
                    break;
                }
            }

            m_output.write(reinterpret_cast<char const*>(&header),
                           sizeof(header));
            if (!record.payload.empty())
            {
                m_output.write(
                    reinterpret_cast<char const*>(record.payload.data()),
                    static_cast<std::streamsize>(record.payload.size()));
            }
            if (!m_output)
            {
                Fail("Measurement recording write failed");
                break;
            }

            m_current_chunk_bytes += record_bytes;
            m_bytes_written.fetch_add(record_bytes,
                                      std::memory_order_relaxed);
            switch (record.type)
            {
            case MeasurementSessionRecordType::snapshot:
                m_snapshot_records.fetch_add(1u, std::memory_order_relaxed);
                break;
            case MeasurementSessionRecordType::measurement:
                m_measurement_records.fetch_add(1u,
                                                std::memory_order_relaxed);
                break;
            case MeasurementSessionRecordType::catalog:
                m_catalog_records.fetch_add(1u, std::memory_order_relaxed);
                break;
            case MeasurementSessionRecordType::event:
                m_event_records.fetch_add(1u, std::memory_order_relaxed);
                break;
            default:
                break;
            }
        }
        if (m_output.is_open())
        {
            m_output.flush();
        }
    }

    bool MeasurementSessionRecorder::OpenChunk(uint32_t index,
                                               std::string& error)
    {
        auto const path = m_session_path / chunk_filename(index);
        m_output.open(path, std::ios::binary | std::ios::trunc);
        if (!m_output)
        {
            error = "Could not open recording chunk " + path.string();
            return false;
        }

        MeasurementSessionChunkHeader header{};
        std::memcpy(header.magic, "DABMSR1", sizeof("DABMSR1"));
        header.format_version = measurement_session_format_version;
        header.chunk_index = index;
        header.session_start_unix_ns = m_started_unix_ns;
        m_output.write(reinterpret_cast<char const*>(&header),
                       sizeof(header));
        if (!m_output)
        {
            error = "Could not write recording chunk header";
            m_output.close();
            return false;
        }
        m_current_chunk_index = index;
        m_current_chunk_bytes = sizeof(header);
        m_bytes_written.fetch_add(sizeof(header),
                                  std::memory_order_relaxed);
        m_chunk_count.store(index, std::memory_order_release);
        return true;
    }

    void MeasurementSessionRecorder::Fail(std::string message) noexcept
    {
        m_active.store(false, std::memory_order_release);
        m_failed.store(true, std::memory_order_release);
        std::scoped_lock lock{ m_mutex };
        m_error = std::move(message);
        m_dropped_records.fetch_add(m_queue.size(),
                                    std::memory_order_relaxed);
        m_queue.clear();
        m_queued_bytes = 0u;
        m_stop_requested = true;
    }

    void MeasurementSessionRecorder::WriteManifest(char const* state) noexcept
    {
        try
        {
            auto const path = m_session_path / "session.json";
            auto temporary = path;
            temporary += ".tmp";
            std::ofstream manifest{ temporary, std::ios::trunc };
            if (!manifest)
            {
                return;
            }
            auto const status = Status();
            manifest << "{\n"
                     << "  \"format\": \"DABMSR\",\n"
                     << "  \"format_version\": "
                     << measurement_session_format_version << ",\n"
                     << "  \"state\": \"" << state << "\",\n"
                     << "  \"application_version\": \""
                     << json_escape(m_info.application_version) << "\",\n"
                     << "  \"started_unix_ns\": " << m_started_unix_ns << ",\n"
                     << "  \"elapsed_ns\": " << status.elapsed_ns << ",\n"
                     << "  \"record_timestamps\": \"capture_elapsed_ns\",\n"
                     << "  \"context\": \"" << json_escape(m_info.context)
                     << "\",\n"
                     << "  \"device\": \"" << json_escape(m_info.device)
                     << "\",\n"
                     << "  \"driver\": \"" << json_escape(m_info.driver)
                     << "\",\n"
                     << "  \"serial\": \"" << json_escape(m_info.serial)
                     << "\",\n"
                     << "  \"channel\": \"" << json_escape(m_info.channel)
                     << "\",\n"
                     << "  \"ensemble\": \"" << json_escape(m_info.ensemble)
                     << "\",\n"
                     << "  \"service\": \"" << json_escape(m_info.service)
                     << "\",\n"
                     << "  \"frequency_hz\": " << m_info.frequency_hz << ",\n"
                     << "  \"sample_rate_hz\": " << m_info.sample_rate_hz << ",\n"
                     << "  \"gain_reduction_db\": "
                     << m_info.gain_reduction_db << ",\n"
                     << "  \"agc_enabled\": "
                     << (m_info.agc_enabled ? "true" : "false") << ",\n"
                     << "  \"calibration_profile_id\": \""
                     << json_escape(m_info.calibration_profile_id) << "\",\n"
                     << "  \"continuous_iq\": false,\n"
                     << "  \"decoded_audio_payload\": false,\n"
                     << "  \"snapshot_schema_bytes\": "
                     << sizeof(dab_receive_snapshot_t) << ",\n"
                     << "  \"catalog_schema_bytes\": "
                     << sizeof(dab_receive_catalog_t) << ",\n"
                     << "  \"chunks\": " << status.chunk_count << ",\n"
                     << "  \"bytes_written\": " << status.bytes_written << ",\n"
                     << "  \"dropped_records\": " << status.dropped_records << ",\n"
                     << "  \"snapshot_records\": " << status.snapshot_records << ",\n"
                     << "  \"measurement_records\": "
                     << status.measurement_records << ",\n"
                     << "  \"catalog_records\": " << status.catalog_records << ",\n"
                     << "  \"event_records\": " << status.event_records << ",\n"
                     << "  \"error\": \"" << json_escape(status.error) << "\"\n"
                     << "}\n";
            manifest.flush();
            manifest.close();
            std::error_code error;
            std::filesystem::remove(path, error);
            error.clear();
            std::filesystem::rename(temporary, path, error);
        }
        catch (...)
        {
        }
    }

    uint64_t MeasurementSessionRecorder::ElapsedNs() const noexcept
    {
        if (m_started_unix_ns == 0u)
        {
            return 0u;
        }
        return static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() -
                m_started_steady).count());
    }

    std::string MeasurementSessionRecorder::MetadataJson() const
    {
        std::ostringstream metadata;
        metadata << "{\"context\":\"" << json_escape(m_info.context)
                 << "\",\"continuous_iq\":false,"
                 << "\"decoded_audio_payload\":false,"
                 << "\"measurement_arrays\":["
                 << "\"spectrum\",\"constellation\",\"cir\","
                 << "\"channel_response\",\"carrier_mer\"]}";
        return metadata.str();
    }
}
