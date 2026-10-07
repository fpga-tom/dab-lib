#include "MeasurementSessionReplay.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstring>
#include <fstream>
#include <iterator>
#include <limits>
#include <cmath>
#include <optional>
#include <sstream>

namespace
{
    constexpr uint32_t maximum_replay_payload_bytes = 64u * 1024u * 1024u;

    std::optional<std::string> json_string(std::string const& json,
                                           std::string const& name)
    {
        std::string const key = "\"" + name + "\"";
        size_t position = json.find(key);
        if (position == std::string::npos)
        {
            return std::nullopt;
        }
        position = json.find(':', position + key.size());
        if (position == std::string::npos)
        {
            return std::nullopt;
        }
        position = json.find('"', position + 1u);
        if (position == std::string::npos)
        {
            return std::nullopt;
        }

        std::string result;
        for (++position; position < json.size(); ++position)
        {
            char const character = json[position];
            if (character == '"')
            {
                return result;
            }
            if (character != '\\')
            {
                result.push_back(character);
                continue;
            }
            if (++position >= json.size())
            {
                return std::nullopt;
            }
            switch (json[position])
            {
            case '"':
            case '\\':
            case '/':
                result.push_back(json[position]);
                break;
            case 'b':
                result.push_back('\b');
                break;
            case 'f':
                result.push_back('\f');
                break;
            case 'n':
                result.push_back('\n');
                break;
            case 'r':
                result.push_back('\r');
                break;
            case 't':
                result.push_back('\t');
                break;
            default:
                return std::nullopt;
            }
        }
        return std::nullopt;
    }

    std::optional<uint64_t> json_unsigned(std::string const& json,
                                          std::string const& name)
    {
        std::string const key = "\"" + name + "\"";
        size_t position = json.find(key);
        if (position == std::string::npos)
        {
            return std::nullopt;
        }
        position = json.find(':', position + key.size());
        if (position == std::string::npos)
        {
            return std::nullopt;
        }
        ++position;
        while (position < json.size() &&
               (json[position] == ' ' || json[position] == '\t' ||
                json[position] == '\r' || json[position] == '\n'))
        {
            ++position;
        }
        uint64_t result = 0u;
        auto const conversion = std::from_chars(
            json.data() + position,
            json.data() + json.size(),
            result);
        if (conversion.ec != std::errc{})
        {
            return std::nullopt;
        }
        return result;
    }

    bool valid_record_type(uint16_t type)
    {
        return type >= static_cast<uint16_t>(
                           dab::measurement::MeasurementSessionRecordType::metadata) &&
               type <= static_cast<uint16_t>(
                           dab::measurement::MeasurementSessionRecordType::event);
    }

    bool valid_snapshot_payload(std::vector<uint8_t> const& payload)
    {
        if (payload.size() != sizeof(dab_receive_snapshot_t))
        {
            return false;
        }
        dab_receive_snapshot_t snapshot{};
        std::memcpy(&snapshot, payload.data(), sizeof(snapshot));
        if (snapshot.fic_fig_stat_count >
                DAB_RECEIVE_FIC_FIG_STAT_CAPACITY ||
            snapshot.msc_raw_data_length >
                DAB_RECEIVE_MSC_RAW_CAPACITY ||
            snapshot.msc_dabplus_au_count >
                DAB_RECEIVE_DABPLUS_MAX_AUS ||
            snapshot.msc_dabplus_history_count >
                DAB_RECEIVE_DABPLUS_HISTORY_CAPACITY ||
            snapshot.audio_au_history_count >
                DAB_RECEIVE_AUDIO_AU_HISTORY_CAPACITY)
        {
            return false;
        }
        for (uint32_t index = 0u;
             index < snapshot.fic_fig_stat_count;
             ++index)
        {
            if (snapshot.fic_fig_stats[index].raw_length >
                    DAB_RECEIVE_FIC_FIG_RAW_CAPACITY ||
                snapshot.fic_fig_stats[index].previous_raw_length >
                    DAB_RECEIVE_FIC_FIG_RAW_CAPACITY)
            {
                return false;
            }
        }
        for (uint32_t index = 0u;
             index < snapshot.msc_dabplus_au_count;
             ++index)
        {
            if (snapshot.msc_dabplus_au_preview_length[index] >
                DAB_RECEIVE_AUDIO_AU_PREVIEW_CAPACITY)
            {
                return false;
            }
        }
        for (uint32_t index = 0u;
             index < snapshot.msc_dabplus_history_count;
             ++index)
        {
            if (snapshot.msc_dabplus_history[index].au_count >
                DAB_RECEIVE_DABPLUS_MAX_AUS)
            {
                return false;
            }
        }
        for (uint32_t index = 0u;
             index < snapshot.audio_au_history_count;
             ++index)
        {
            if (snapshot.audio_au_history[index].preview_length >
                DAB_RECEIVE_AUDIO_AU_PREVIEW_CAPACITY)
            {
                return false;
            }
        }
        return true;
    }

    bool valid_catalog_payload(std::vector<uint8_t> const& payload)
    {
        if (payload.size() != sizeof(dab_receive_catalog_t))
        {
            return false;
        }
        dab_receive_catalog_t catalog{};
        std::memcpy(&catalog, payload.data(), sizeof(catalog));
        if (catalog.service_count > DAB_RECEIVE_CATALOG_MAX_SERVICES ||
            catalog.subchannel_count >
                DAB_RECEIVE_CATALOG_MAX_SUBCHANNELS)
        {
            return false;
        }
        for (uint32_t index = 0u;
             index < catalog.service_count;
             ++index)
        {
            if (catalog.services[index].component_count >
                DAB_RECEIVE_CATALOG_MAX_COMPONENTS_PER_SERVICE)
            {
                return false;
            }
        }
        return true;
    }

    bool valid_measurement_payload(std::vector<uint8_t> const& payload)
    {
        if (payload.size() < sizeof(
                dab::measurement::MeasurementSessionFrameHeader))
        {
            return false;
        }
        dab::measurement::MeasurementSessionFrameHeader header{};
        std::memcpy(&header, payload.data(), sizeof(header));
        if (header.spectrum_bin_count >
                DAB_RECEIVE_SPECTRUM_BIN_COUNT ||
            header.constellation_point_count >
                DAB_RECEIVE_CONSTELLATION_POINT_CAPACITY ||
            header.cir_bin_count > DAB_RECEIVE_CIR_BIN_COUNT ||
            header.channel_response_count >
                DAB_RECEIVE_CHANNEL_RESPONSE_COUNT ||
            header.carrier_mer_count >
                DAB_RECEIVE_CARRIER_MER_COUNT)
        {
            return false;
        }
        size_t const float_count =
            header.spectrum_bin_count +
            2u * static_cast<size_t>(
                header.constellation_point_count) +
            header.cir_bin_count +
            2u * static_cast<size_t>(
                header.channel_response_count) +
            header.carrier_mer_count;
        return payload.size() ==
            sizeof(header) + float_count * sizeof(float);
    }

    template <typename Value>
    bool read_value(uint8_t const*& cursor,
                    uint8_t const* end,
                    Value& value)
    {
        if (static_cast<size_t>(end - cursor) < sizeof(Value))
        {
            return false;
        }
        std::memcpy(&value, cursor, sizeof(Value));
        cursor += sizeof(Value);
        return true;
    }

    template <typename Value, size_t Capacity>
    bool read_values(uint8_t const*& cursor,
                     uint8_t const* end,
                     std::array<Value, Capacity>& destination,
                     uint32_t count)
    {
        if (count > Capacity)
        {
            return false;
        }
        size_t const bytes = static_cast<size_t>(count) * sizeof(Value);
        if (static_cast<size_t>(end - cursor) < bytes)
        {
            return false;
        }
        if (bytes != 0u)
        {
            std::memcpy(destination.data(), cursor, bytes);
            cursor += bytes;
        }
        return true;
    }
}

namespace dab::measurement
{
    bool MeasurementSessionReplay::ReadSessionInfo(
        std::filesystem::path const& selected_path,
        MeasurementSessionReplayInfo& info,
        std::string& error)
    {
        info = {};
        error.clear();
        try
        {
            std::error_code filesystem_error;
            std::filesystem::path session_path = selected_path;
            if (std::filesystem::is_regular_file(session_path,
                                                 filesystem_error))
            {
                session_path = session_path.parent_path();
            }
            if (filesystem_error ||
                !std::filesystem::is_directory(session_path,
                                               filesystem_error))
            {
                error = "The selected recording folder does not exist";
                return false;
            }
            std::ifstream manifest_input{
                session_path / "session.json",
                std::ios::binary
            };
            if (!manifest_input)
            {
                error = "The selected folder does not contain session.json";
                return false;
            }
            std::string const manifest{
                std::istreambuf_iterator<char>{ manifest_input },
                std::istreambuf_iterator<char>{}
            };
            if (json_string(manifest, "format").value_or("") != "DABMSR")
            {
                error = "The selected folder is not a DAB measurement recording";
                return false;
            }
            auto const version = json_unsigned(manifest, "format_version");
            if (!version ||
                *version != measurement_session_format_version)
            {
                error = "Unsupported measurement recording format version";
                return false;
            }

            info.session_path = std::move(session_path);
            info.state = json_string(manifest, "state").value_or("unknown");
            info.context = json_string(manifest, "context").value_or("");
            info.device = json_string(manifest, "device").value_or("");
            info.driver = json_string(manifest, "driver").value_or("");
            info.serial = json_string(manifest, "serial").value_or("");
            info.channel = json_string(manifest, "channel").value_or("");
            info.ensemble =
                json_string(manifest, "ensemble").value_or("");
            info.service = json_string(manifest, "service").value_or("");
            info.frequency_hz =
                json_unsigned(manifest, "frequency_hz").value_or(0u);
            info.sample_rate_hz = static_cast<uint32_t>(
                (std::min)(json_unsigned(manifest, "sample_rate_hz")
                               .value_or(0u),
                           static_cast<uint64_t>(
                               (std::numeric_limits<uint32_t>::max)())));
            info.started_unix_ns =
                json_unsigned(manifest, "started_unix_ns").value_or(0u);
            info.duration_ns =
                json_unsigned(manifest, "elapsed_ns").value_or(0u);
            info.bytes_written =
                json_unsigned(manifest, "bytes_written").value_or(0u);
            info.dropped_records =
                json_unsigned(manifest, "dropped_records").value_or(0u);
            info.snapshot_records =
                json_unsigned(manifest, "snapshot_records").value_or(0u);
            info.measurement_records =
                json_unsigned(manifest, "measurement_records").value_or(0u);
            info.catalog_records =
                json_unsigned(manifest, "catalog_records").value_or(0u);
            info.event_records =
                json_unsigned(manifest, "event_records").value_or(0u);
            info.chunk_count = static_cast<uint32_t>(
                (std::min)(json_unsigned(manifest, "chunks").value_or(0u),
                           static_cast<uint64_t>(
                               (std::numeric_limits<uint32_t>::max)())));
            info.record_count =
                info.snapshot_records +
                info.measurement_records +
                info.catalog_records +
                info.event_records +
                1u;
            return true;
        }
        catch (std::exception const& exception)
        {
            error = std::string{ "Could not inspect recording: " } +
                exception.what();
        }
        catch (...)
        {
            error = "Could not inspect recording";
        }
        info = {};
        return false;
    }

    bool MeasurementSessionReplay::Open(
        std::filesystem::path const& selected_path,
        std::string& error)
    {
        Close();
        error.clear();
        try
        {
            std::error_code filesystem_error;
            std::filesystem::path session_path = selected_path;
            if (std::filesystem::is_regular_file(session_path,
                                                 filesystem_error))
            {
                session_path = session_path.parent_path();
            }
            if (filesystem_error ||
                !std::filesystem::is_directory(session_path,
                                               filesystem_error))
            {
                error = "The selected recording folder does not exist";
                return false;
            }

            auto const manifest_path = session_path / "session.json";
            std::ifstream manifest_input{ manifest_path, std::ios::binary };
            if (!manifest_input)
            {
                error = "The selected folder does not contain session.json";
                return false;
            }
            std::string const manifest{
                std::istreambuf_iterator<char>{ manifest_input },
                std::istreambuf_iterator<char>{}
            };
            if (json_string(manifest, "format").value_or("") != "DABMSR")
            {
                error = "The selected folder is not a DAB measurement recording";
                return false;
            }
            auto const version = json_unsigned(manifest, "format_version");
            if (!version ||
                *version != measurement_session_format_version)
            {
                error = "Unsupported measurement recording format version";
                return false;
            }

            m_info.session_path = session_path;
            m_info.state = json_string(manifest, "state").value_or("unknown");
            m_info.context = json_string(manifest, "context").value_or("");
            m_info.device = json_string(manifest, "device").value_or("");
            m_info.driver = json_string(manifest, "driver").value_or("");
            m_info.serial = json_string(manifest, "serial").value_or("");
            m_info.channel = json_string(manifest, "channel").value_or("");
            m_info.ensemble =
                json_string(manifest, "ensemble").value_or("");
            m_info.service = json_string(manifest, "service").value_or("");
            m_info.frequency_hz =
                json_unsigned(manifest, "frequency_hz").value_or(0u);
            m_info.sample_rate_hz = static_cast<uint32_t>(
                (std::min)(json_unsigned(manifest, "sample_rate_hz")
                               .value_or(0u),
                           static_cast<uint64_t>(
                               (std::numeric_limits<uint32_t>::max)())));
            m_info.started_unix_ns =
                json_unsigned(manifest, "started_unix_ns").value_or(0u);
            m_info.bytes_written =
                json_unsigned(manifest, "bytes_written").value_or(0u);
            m_info.dropped_records =
                json_unsigned(manifest, "dropped_records").value_or(0u);
            uint64_t const manifest_duration =
                json_unsigned(manifest, "elapsed_ns").value_or(0u);
            bool const capture_elapsed_timestamps =
                json_string(manifest, "record_timestamps")
                    .value_or("") == "capture_elapsed_ns";

            for (auto const& entry :
                 std::filesystem::directory_iterator(session_path))
            {
                if (!entry.is_regular_file())
                {
                    continue;
                }
                std::string const name = entry.path().filename().string();
                if (name.rfind("data-", 0u) == 0u &&
                    entry.path().extension() == ".dabm")
                {
                    m_chunks.push_back(entry.path());
                }
            }
            std::sort(m_chunks.begin(), m_chunks.end());
            if (m_chunks.empty())
            {
                error = "The recording does not contain any data chunks";
                Close();
                return false;
            }

            std::vector<uint8_t> payload;
            uint32_t expected_chunk_index = 1u;
            for (size_t chunk_slot = 0u;
                 chunk_slot < m_chunks.size();
                 ++chunk_slot)
            {
                std::ifstream input{ m_chunks[chunk_slot],
                                     std::ios::binary };
                if (!input)
                {
                    error = "Could not open recording chunk " +
                        m_chunks[chunk_slot].filename().string();
                    Close();
                    return false;
                }

                MeasurementSessionChunkHeader chunk{};
                input.read(reinterpret_cast<char*>(&chunk), sizeof(chunk));
                if (!input ||
                    std::memcmp(chunk.magic, "DABMSR1", 7u) != 0 ||
                    chunk.format_version != measurement_session_format_version ||
                    chunk.chunk_index != expected_chunk_index)
                {
                    error = "Invalid or out-of-order recording chunk " +
                        m_chunks[chunk_slot].filename().string();
                    Close();
                    return false;
                }
                ++expected_chunk_index;

                while (input.peek() != std::char_traits<char>::eof())
                {
                    MeasurementSessionRecordHeader header{};
                    input.read(reinterpret_cast<char*>(&header),
                               sizeof(header));
                    if (!input ||
                        header.magic != measurement_session_record_magic ||
                        header.header_version != 1u ||
                        !valid_record_type(header.record_type))
                    {
                        error = "Invalid record header in " +
                            m_chunks[chunk_slot].filename().string();
                        Close();
                        return false;
                    }
                    if (header.payload_size > maximum_replay_payload_bytes)
                    {
                        error = "Recording record exceeds the safe import size";
                        Close();
                        return false;
                    }

                    uint64_t const payload_offset =
                        static_cast<uint64_t>(input.tellg());
                    payload.resize(header.payload_size);
                    if (!payload.empty())
                    {
                        input.read(
                            reinterpret_cast<char*>(payload.data()),
                            static_cast<std::streamsize>(payload.size()));
                    }
                    if (!input)
                    {
                        error = "Truncated record payload in " +
                            m_chunks[chunk_slot].filename().string();
                        Close();
                        return false;
                    }
                    if (MeasurementSessionRecorder::Crc32(
                            payload.data(),
                            payload.size()) != header.payload_crc32)
                    {
                        error = "CRC mismatch in " +
                            m_chunks[chunk_slot].filename().string();
                        Close();
                        return false;
                    }

                    auto const type =
                        static_cast<MeasurementSessionRecordType>(
                            header.record_type);
                    bool const schema_valid =
                        type == MeasurementSessionRecordType::snapshot
                            ? valid_snapshot_payload(payload)
                            : type == MeasurementSessionRecordType::measurement
                                ? valid_measurement_payload(payload)
                                : type == MeasurementSessionRecordType::catalog
                                    ? valid_catalog_payload(payload)
                                    : true;
                    if (!schema_valid)
                    {
                        error = "Record schema is invalid or incompatible in " +
                            m_chunks[chunk_slot].filename().string();
                        Close();
                        return false;
                    }

                    IndexEntry index{
                        type,
                        header.timestamp_ns,
                        header.timestamp_ns,
                        header.sequence,
                        header.payload_size,
                        header.payload_crc32,
                        chunk_slot,
                        payload_offset
                    };
                    m_index.push_back(index);
                    switch (index.type)
                    {
                    case MeasurementSessionRecordType::snapshot:
                        ++m_info.snapshot_records;
                        break;
                    case MeasurementSessionRecordType::measurement:
                        ++m_info.measurement_records;
                        break;
                    case MeasurementSessionRecordType::catalog:
                        ++m_info.catalog_records;
                        break;
                    case MeasurementSessionRecordType::event:
                        ++m_info.event_records;
                        break;
                    default:
                        break;
                    }
                }
            }

            if (m_index.empty())
            {
                error = "The recording contains no records";
                Close();
                return false;
            }

            if (!capture_elapsed_timestamps)
            {
                uint64_t previous = 0u;
                uint64_t previous_receiver_raw = 0u;
                uint64_t previous_receiver_time = 0u;
                bool have_receiver_time = false;
                for (auto& entry : m_index)
                {
                    bool const receiver_record =
                        entry.type ==
                            MeasurementSessionRecordType::snapshot ||
                        entry.type ==
                            MeasurementSessionRecordType::measurement;
                    uint64_t normalized = entry.raw_timestamp_ns;
                    if (receiver_record)
                    {
                        if (!have_receiver_time)
                        {
                            normalized = previous;
                            have_receiver_time = true;
                        }
                        else if (entry.raw_timestamp_ns >=
                                     previous_receiver_raw &&
                                 entry.raw_timestamp_ns -
                                     previous_receiver_raw <=
                                     5000000000ull)
                        {
                            uint64_t const delta =
                                entry.raw_timestamp_ns -
                                previous_receiver_raw;
                            normalized =
                                delta >
                                    (std::numeric_limits<uint64_t>::max)() -
                                        previous_receiver_time
                                    ? (std::numeric_limits<uint64_t>::max)()
                                    : previous_receiver_time + delta;
                        }
                        else
                        {
                            // Receiver timestamps restarted during watchdog
                            // recovery. Preserve file order and allow the next
                            // capture-elapsed event to anchor the new segment.
                            normalized = previous;
                        }
                        previous_receiver_raw = entry.raw_timestamp_ns;
                        previous_receiver_time = (std::max)(
                            normalized,
                            previous);
                    }
                    entry.timestamp_ns = (std::max)(normalized, previous);
                    previous = entry.timestamp_ns;
                }
            }

            m_info.record_count = m_index.size();
            m_info.chunk_count =
                static_cast<uint32_t>(m_chunks.size());
            m_info.duration_ns = (std::max)(
                manifest_duration,
                m_index.back().timestamp_ns);
            m_cursor = 0u;
            m_open = true;
            return true;
        }
        catch (std::exception const& exception)
        {
            error = std::string{ "Could not import recording: " } +
                exception.what();
        }
        catch (...)
        {
            error = "Could not import recording";
        }
        Close();
        return false;
    }

    void MeasurementSessionReplay::Close() noexcept
    {
        if (m_read_stream.is_open())
        {
            m_read_stream.close();
        }
        m_read_stream.clear();
        m_read_chunk = (std::numeric_limits<size_t>::max)();
        m_info = {};
        m_chunks.clear();
        m_index.clear();
        m_cursor = 0u;
        m_open = false;
    }

    bool MeasurementSessionReplay::IsOpen() const noexcept
    {
        return m_open;
    }

    MeasurementSessionReplayInfo const&
    MeasurementSessionReplay::Info() const noexcept
    {
        return m_info;
    }

    bool MeasurementSessionReplay::BuildTimeline(
        size_t maximum_points,
        std::vector<MeasurementSessionTimelinePoint>& timeline,
        std::vector<MeasurementSessionReplayEvent>& events,
        std::string& error) const
    {
        timeline.clear();
        events.clear();
        error.clear();
        if (!m_open)
        {
            error = "No recording is open";
            return false;
        }
        maximum_points = (std::max)(maximum_points, size_t{ 2u });
        size_t const snapshot_count =
            static_cast<size_t>(m_info.snapshot_records);
        size_t const stride = (std::max)(
            size_t{ 1u },
            (snapshot_count + maximum_points - 1u) / maximum_points);
        size_t snapshot_index = 0u;
        std::vector<std::pair<uint64_t, float>> mer_samples;
        mer_samples.reserve(static_cast<size_t>(
            m_info.measurement_records));

        for (size_t index = 0u; index < m_index.size(); ++index)
        {
            auto const type = m_index[index].type;
            if (type == MeasurementSessionRecordType::measurement)
            {
                MeasurementSessionReplayRecord record;
                if (!ReadRecord(index, record, error))
                {
                    return false;
                }
                MeasurementFrame frame;
                if (!DecodeMeasurement(record, frame, error))
                {
                    return false;
                }
                double sum = 0.0;
                uint32_t count = 0u;
                for (uint32_t carrier = 0u;
                     carrier < frame.carrier_mer_count;
                     ++carrier)
                {
                    float const value = frame.carrier_mer_db[carrier];
                    if (std::isfinite(value))
                    {
                        sum += value;
                        ++count;
                    }
                }
                if (count != 0u)
                {
                    mer_samples.emplace_back(
                        record.timestamp_ns,
                        static_cast<float>(sum / count));
                }
                continue;
            }
            if (type == MeasurementSessionRecordType::event)
            {
                if (events.size() >= 2000u)
                {
                    continue;
                }
                MeasurementSessionReplayRecord record;
                if (!ReadRecord(index, record, error))
                {
                    return false;
                }
                MeasurementSessionReplayEvent event;
                event.timestamp_ns = record.timestamp_ns;
                if (!DecodeEvent(record,
                                 event.event_type,
                                 event.message,
                                 error))
                {
                    return false;
                }
                events.push_back(std::move(event));
                continue;
            }
            if (type != MeasurementSessionRecordType::snapshot)
            {
                continue;
            }

            bool const retain =
                (snapshot_index % stride) == 0u ||
                snapshot_index + 1u == snapshot_count;
            ++snapshot_index;
            if (!retain)
            {
                continue;
            }
            MeasurementSessionReplayRecord record;
            if (!ReadRecord(index, record, error))
            {
                return false;
            }
            dab_receive_snapshot_t snapshot{};
            if (!DecodeSnapshot(record, snapshot, error))
            {
                return false;
            }

            MeasurementSessionTimelinePoint point;
            point.timestamp_ns = record.timestamp_ns;
            point.power_valid =
                snapshot.calibrated_power_valid != 0 ||
                snapshot.adc_level_valid != 0;
            point.power_calibrated =
                snapshot.calibrated_power_valid != 0;
            point.power = point.power_calibrated
                ? snapshot.calibrated_power_dbm
                : snapshot.adc_level_dbfs;
            point.cn_valid = snapshot.cn_valid != 0;
            point.cn_db = snapshot.cn_db;
            point.cfo_valid = snapshot.cfo_valid != 0;
            point.cfo_hz = snapshot.cfo_hz;
            point.msc_ber_valid = snapshot.msc_ber_valid != 0;
            point.msc_ber = snapshot.msc_ber;
            point.clipping_valid =
                snapshot.adc_window_sample_count != 0u;
            point.clipping_percent =
                snapshot.adc_clipping_percent;
            point.dropped_blocks = snapshot.dropped_blocks;
            point.locked = snapshot.locked != 0;
            timeline.push_back(point);
        }

        for (auto& point : timeline)
        {
            if (mer_samples.empty())
            {
                break;
            }
            auto iterator = std::lower_bound(
                mer_samples.begin(),
                mer_samples.end(),
                point.timestamp_ns,
                [](auto const& sample, uint64_t timestamp)
                {
                    return sample.first < timestamp;
                });
            auto selected = iterator;
            if (iterator == mer_samples.end())
            {
                selected = std::prev(iterator);
            }
            else if (iterator != mer_samples.begin())
            {
                auto const previous = std::prev(iterator);
                if (point.timestamp_ns - previous->first <=
                    iterator->first - point.timestamp_ns)
                {
                    selected = previous;
                }
            }
            point.mer_valid = true;
            point.mer_db = selected->second;
        }
        return true;
    }

    bool MeasurementSessionReplay::Seek(
        uint64_t timestamp_ns,
        std::vector<MeasurementSessionReplayRecord>& state,
        std::string& error)
    {
        state.clear();
        error.clear();
        if (!m_open)
        {
            error = "No recording is open";
            return false;
        }
        timestamp_ns = (std::min)(timestamp_ns, m_info.duration_ns);

        std::array<std::optional<size_t>, 3u> latest;
        for (size_t index = 0u;
             index < m_index.size() &&
             m_index[index].timestamp_ns <= timestamp_ns;
             ++index)
        {
            switch (m_index[index].type)
            {
            case MeasurementSessionRecordType::catalog:
                latest[0] = index;
                break;
            case MeasurementSessionRecordType::snapshot:
                latest[1] = index;
                break;
            case MeasurementSessionRecordType::measurement:
                latest[2] = index;
                break;
            default:
                break;
            }
        }

        for (auto const index : latest)
        {
            if (index)
            {
                MeasurementSessionReplayRecord record;
                if (!ReadRecord(*index, record, error))
                {
                    return false;
                }
                state.push_back(std::move(record));
            }
        }
        m_cursor = static_cast<size_t>(
            std::lower_bound(
                m_index.begin(),
                m_index.end(),
                timestamp_ns,
                [](IndexEntry const& entry, uint64_t value)
                {
                    return entry.timestamp_ns < value;
                }) - m_index.begin());
        return true;
    }

    bool MeasurementSessionReplay::ReadUntil(
        uint64_t timestamp_ns,
        std::vector<MeasurementSessionReplayRecord>& records,
        size_t maximum_records,
        std::string& error)
    {
        records.clear();
        error.clear();
        if (!m_open)
        {
            error = "No recording is open";
            return false;
        }
        while (m_cursor < m_index.size() &&
               m_index[m_cursor].timestamp_ns <= timestamp_ns &&
               records.size() < maximum_records)
        {
            MeasurementSessionReplayRecord record;
            if (!ReadRecord(m_cursor, record, error))
            {
                return false;
            }
            records.push_back(std::move(record));
            ++m_cursor;
        }
        return true;
    }

    bool MeasurementSessionReplay::DecodeSnapshot(
        MeasurementSessionReplayRecord const& record,
        dab_receive_snapshot_t& snapshot,
        std::string& error)
    {
        if (record.type != MeasurementSessionRecordType::snapshot ||
            record.payload.size() != sizeof(snapshot))
        {
            error = "Snapshot schema does not match this application build";
            return false;
        }
        std::memcpy(&snapshot, record.payload.data(), sizeof(snapshot));
        return true;
    }

    bool MeasurementSessionReplay::DecodeMeasurement(
        MeasurementSessionReplayRecord const& record,
        MeasurementFrame& frame,
        std::string& error)
    {
        if (record.type != MeasurementSessionRecordType::measurement ||
            record.payload.size() < sizeof(MeasurementSessionFrameHeader))
        {
            error = "Invalid measurement frame";
            return false;
        }
        frame = {};
        frame.sequence = record.sequence;
        frame.timestamp_ns = record.timestamp_ns;

        uint8_t const* cursor = record.payload.data();
        uint8_t const* end = cursor + record.payload.size();
        MeasurementSessionFrameHeader header{};
        if (!read_value(cursor, end, header) ||
            header.spectrum_bin_count > DAB_RECEIVE_SPECTRUM_BIN_COUNT ||
            header.constellation_point_count >
                DAB_RECEIVE_CONSTELLATION_POINT_CAPACITY ||
            header.cir_bin_count > DAB_RECEIVE_CIR_BIN_COUNT ||
            header.channel_response_count >
                DAB_RECEIVE_CHANNEL_RESPONSE_COUNT ||
            header.carrier_mer_count > DAB_RECEIVE_CARRIER_MER_COUNT)
        {
            error = "Measurement frame dimensions are invalid";
            return false;
        }

        frame.frequency_hz = header.frequency_hz;
        frame.frame_start_sample = header.frame_start_sample;
        frame.sample_rate_hz = header.sample_rate_hz;
        frame.spectrum_bin_count = header.spectrum_bin_count;
        frame.spectrum_start_hz = header.spectrum_start_hz;
        frame.spectrum_bin_hz = header.spectrum_bin_hz;
        frame.constellation_point_count =
            header.constellation_point_count;
        frame.cir_bin_count = header.cir_bin_count;
        frame.cir_sample_period_ns = header.cir_sample_period_ns;
        frame.channel_response_count =
            header.channel_response_count;
        frame.carrier_mer_count = header.carrier_mer_count;
        frame.first_carrier = header.first_carrier;
        frame.negative_carrier_count =
            header.negative_carrier_count;
        frame.carrier_spacing_hz = header.carrier_spacing_hz;

        if (!read_values(cursor,
                         end,
                         frame.spectrum_dbfs,
                         header.spectrum_bin_count) ||
            !read_values(cursor,
                         end,
                         frame.constellation_i,
                         header.constellation_point_count) ||
            !read_values(cursor,
                         end,
                         frame.constellation_q,
                         header.constellation_point_count) ||
            !read_values(cursor,
                         end,
                         frame.cir_power_db,
                         header.cir_bin_count) ||
            !read_values(cursor,
                         end,
                         frame.channel_magnitude_db,
                         header.channel_response_count) ||
            !read_values(cursor,
                         end,
                         frame.channel_phase_deg,
                         header.channel_response_count) ||
            !read_values(cursor,
                         end,
                         frame.carrier_mer_db,
                         header.carrier_mer_count) ||
            cursor != end)
        {
            error = "Measurement frame payload size is invalid";
            return false;
        }
        return true;
    }

    bool MeasurementSessionReplay::DecodeCatalog(
        MeasurementSessionReplayRecord const& record,
        dab_receive_catalog_t& catalog,
        std::string& error)
    {
        if (record.type != MeasurementSessionRecordType::catalog ||
            record.payload.size() != sizeof(catalog))
        {
            error = "Catalog schema does not match this application build";
            return false;
        }
        std::memcpy(&catalog, record.payload.data(), sizeof(catalog));
        return true;
    }

    bool MeasurementSessionReplay::DecodeEvent(
        MeasurementSessionReplayRecord const& record,
        uint32_t& event_type,
        std::string& message,
        std::string& error)
    {
        if (record.type != MeasurementSessionRecordType::event)
        {
            error = "Record is not an event";
            return false;
        }
        std::string const json(record.payload.begin(),
                               record.payload.end());
        auto const parsed_type = json_unsigned(json, "event_type");
        auto const parsed_message = json_string(json, "message");
        if (!parsed_type || !parsed_message ||
            *parsed_type >
                (std::numeric_limits<uint32_t>::max)())
        {
            error = "Recorded event payload is invalid";
            return false;
        }
        event_type = static_cast<uint32_t>(*parsed_type);
        message = *parsed_message;
        return true;
    }

    bool MeasurementSessionReplay::ReadRecord(
        size_t index,
        MeasurementSessionReplayRecord& record,
        std::string& error) const
    {
        if (index >= m_index.size())
        {
            error = "Recording index is out of range";
            return false;
        }
        auto const& entry = m_index[index];
        if (m_read_chunk != entry.chunk)
        {
            if (m_read_stream.is_open())
            {
                m_read_stream.close();
            }
            m_read_stream.clear();
            m_read_stream.open(m_chunks[entry.chunk], std::ios::binary);
            if (!m_read_stream)
            {
                error = "Could not reopen recording chunk";
                m_read_chunk = (std::numeric_limits<size_t>::max)();
                return false;
            }
            m_read_chunk = entry.chunk;
        }
        m_read_stream.clear();
        m_read_stream.seekg(
            static_cast<std::streamoff>(entry.payload_offset));
        if (!m_read_stream)
        {
            error = "Could not seek in recording chunk";
            return false;
        }
        record.type = entry.type;
        record.timestamp_ns = entry.timestamp_ns;
        record.sequence = entry.sequence;
        record.payload.resize(entry.payload_size);
        if (!record.payload.empty())
        {
            m_read_stream.read(
                reinterpret_cast<char*>(record.payload.data()),
                static_cast<std::streamsize>(record.payload.size()));
        }
        if (!m_read_stream)
        {
            error = "Could not read recording payload";
            return false;
        }
        if (MeasurementSessionRecorder::Crc32(
                record.payload.data(),
                record.payload.size()) != entry.payload_crc32)
        {
            error = "Recording changed after import (CRC mismatch)";
            return false;
        }
        return true;
    }
}
