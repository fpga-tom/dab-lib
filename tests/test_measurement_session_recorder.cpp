#include "MeasurementSessionRecorder.h"

#include <array>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>

extern "C" {
#include "test_support.h"
}

namespace
{
    std::filesystem::path recorder_test_root(char const* name)
    {
        auto const unique = std::chrono::steady_clock::now()
            .time_since_epoch().count();
        return std::filesystem::temp_directory_path() /
            (std::string{ name } + "-" + std::to_string(unique));
    }
}

extern "C" void
test_measurement_session_recorder_writes_crc_checked_records(void)
{
    auto const root = recorder_test_root("dab-measurement-recorder");
    dab::measurement::MeasurementSessionRecorder recorder;
    dab::measurement::MeasurementSessionStartInfo info;
    info.recordings_root = root;
    info.context = "11C / test service";
    info.driver = "sdrplay";
    info.serial = "B000123";
    info.frequency_hz = 220352000u;
    info.sample_rate_hz = 2048000u;
    std::string error;

    TEST_ASSERT(recorder.Start(info, error));

    dab_receive_snapshot_t snapshot{};
    snapshot.sequence = 12u;
    snapshot.timestamp_ns = 1000000u;
    snapshot.frequency_hz = 220352000u;
    snapshot.sample_rate_hz = 2048000u;
    snapshot.locked = 1;
    recorder.RecordSnapshot(snapshot);

    std::array<float, 4u> spectrum{ -70.0f, -60.0f, -50.0f, -65.0f };
    std::array<float, 2u> constellation_i{ -0.707f, 0.707f };
    std::array<float, 2u> constellation_q{ 0.707f, -0.707f };
    std::array<float, 2u> cir{ 0.0f, -8.0f };
    std::array<float, 2u> magnitude{ -1.0f, 1.0f };
    std::array<float, 2u> phase{ -30.0f, 30.0f };
    std::array<float, 2u> mer{ 18.0f, 19.0f };
    dab_receive_measurement_frame_t frame{};
    frame.sequence = 13u;
    frame.timestamp_ns = 2000000u;
    frame.frequency_hz = 220352000u;
    frame.sample_rate_hz = 2048000u;
    frame.spectrum_dbfs = spectrum.data();
    frame.spectrum_bin_count = static_cast<uint32_t>(spectrum.size());
    frame.constellation_i = constellation_i.data();
    frame.constellation_q = constellation_q.data();
    frame.constellation_point_count =
        static_cast<uint32_t>(constellation_i.size());
    frame.cir_power_db = cir.data();
    frame.cir_bin_count = static_cast<uint32_t>(cir.size());
    frame.channel_magnitude_db = magnitude.data();
    frame.channel_phase_deg = phase.data();
    frame.channel_response_count =
        static_cast<uint32_t>(magnitude.size());
    frame.carrier_mer_db = mer.data();
    frame.carrier_mer_count = static_cast<uint32_t>(mer.size());
    recorder.RecordMeasurement(frame);

    dab_receive_catalog_t catalog{};
    catalog.sequence = 4u;
    catalog.frequency_hz = 220352000u;
    recorder.RecordCatalog(catalog);
    recorder.RecordEvent(7u, "operator marker");

    auto const status = recorder.Stop();
    TEST_ASSERT(!status.active);
    TEST_ASSERT(!status.failed);
    TEST_ASSERT(status.snapshot_records == 1u);
    TEST_ASSERT(status.measurement_records == 1u);
    TEST_ASSERT(status.catalog_records == 1u);
    TEST_ASSERT(status.event_records == 1u);
    TEST_ASSERT(status.dropped_records == 0u);
    TEST_ASSERT(std::filesystem::exists(
        status.session_path / "session.json"));

    std::ifstream input{ status.session_path / "data-0001.dabm",
                         std::ios::binary };
    TEST_ASSERT(input.good());
    dab::measurement::MeasurementSessionChunkHeader chunk{};
    input.read(reinterpret_cast<char*>(&chunk), sizeof(chunk));
    TEST_ASSERT(input.good());
    TEST_ASSERT(std::memcmp(chunk.magic, "DABMSR1", 7u) == 0);
    TEST_ASSERT(chunk.format_version ==
                dab::measurement::measurement_session_format_version);

    uint32_t record_count = 0u;
    while (input.peek() != std::char_traits<char>::eof())
    {
        dab::measurement::MeasurementSessionRecordHeader header{};
        input.read(reinterpret_cast<char*>(&header), sizeof(header));
        TEST_ASSERT(input.good());
        TEST_ASSERT(header.magic ==
                    dab::measurement::measurement_session_record_magic);
        std::vector<uint8_t> payload(header.payload_size);
        input.read(reinterpret_cast<char*>(payload.data()),
                   static_cast<std::streamsize>(payload.size()));
        TEST_ASSERT(input.good());
        TEST_ASSERT(
            dab::measurement::MeasurementSessionRecorder::Crc32(
                payload.data(),
                payload.size()) == header.payload_crc32);
        ++record_count;
    }
    TEST_ASSERT(record_count == 5u);
    input.close();

    std::error_code cleanup_error;
    std::filesystem::remove_all(root, cleanup_error);
    TEST_ASSERT(!cleanup_error);
}

extern "C" void
test_measurement_session_recorder_drops_oversized_record_without_stopping(void)
{
    auto const root = recorder_test_root("dab-measurement-recorder-overflow");
    dab::measurement::MeasurementSessionRecorderLimits limits;
    limits.maximum_queue_bytes = 1024u;
    dab::measurement::MeasurementSessionRecorder recorder{ limits };
    dab::measurement::MeasurementSessionStartInfo info;
    info.recordings_root = root;
    std::string error;

    TEST_ASSERT(recorder.Start(info, error));
    recorder.RecordEvent(1u, std::string(4096u, 'x'));
    auto live = recorder.Status();
    TEST_ASSERT(live.active);
    TEST_ASSERT(live.dropped_records >= 1u);

    auto const stopped = recorder.Stop();
    TEST_ASSERT(!stopped.failed);
    TEST_ASSERT(stopped.dropped_records >= 1u);

    std::error_code cleanup_error;
    std::filesystem::remove_all(root, cleanup_error);
    TEST_ASSERT(!cleanup_error);
}
