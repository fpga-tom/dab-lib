#include "pch.h"
#include "CalibrationProfileStore.h"
#include "MainWindow.xaml.h"

#include <cstring>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>
#include <winrt/Windows.System.h>

extern "C" {
#include "dab/core/fic.h"
}

#if __has_include("MainWindow.g.cpp")
#include "MainWindow.g.cpp"
#endif

using namespace winrt;
using namespace Windows::Foundation;
using namespace Windows::UI;
using namespace Microsoft::UI::Xaml;
using namespace Microsoft::UI::Xaml::Controls;
using namespace Microsoft::UI::Xaml::Controls::Primitives;
using namespace Microsoft::UI::Xaml::Input;
using namespace Microsoft::UI::Xaml::Media;
using namespace Microsoft::UI::Xaml::Shapes;

namespace
{
    struct resume_dispatcher_queue
    {
        Microsoft::UI::Dispatching::DispatcherQueue dispatcher;

        bool await_ready() const noexcept
        {
            return dispatcher.HasThreadAccess();
        }

        bool await_suspend(std::coroutine_handle<> continuation) const noexcept
        {
            return dispatcher.TryEnqueue(
                [continuation]() mutable
                {
                    continuation.resume();
                });
        }

        bool await_resume() const noexcept
        {
            return dispatcher.HasThreadAccess();
        }
    };

    struct scoped_true
    {
        explicit scoped_true(bool& value) noexcept
            : target(value),
              previous(value)
        {
            target = true;
        }

        ~scoped_true()
        {
            target = previous;
        }

        bool& target;
        bool previous;
    };

    std::wstring application_icon_path()
    {
        std::wstring module_path(32768u, L'\0');
        DWORD const length = GetModuleFileNameW(
            nullptr,
            module_path.data(),
            static_cast<DWORD>(module_path.size()));
        if (length == 0u || length >= module_path.size())
        {
            return {};
        }

        module_path.resize(length);
        size_t const separator = module_path.find_last_of(L"\\/");
        if (separator == std::wstring::npos)
        {
            return {};
        }
        module_path.resize(separator + 1u);
        module_path += L"Assets\\DabMeasurementIcon.ico";
        return module_path;
    }

    std::filesystem::path measurement_recordings_root()
    {
        wchar_t profile[32768]{};
        DWORD const length = GetEnvironmentVariableW(
            L"USERPROFILE",
            profile,
            static_cast<DWORD>(std::size(profile)));
        std::filesystem::path root =
            length != 0u && length < std::size(profile)
                ? std::filesystem::path{ profile } / L"Documents"
                : std::filesystem::temp_directory_path();
        return root / L"DAB Measurement Workbench" / L"Recordings";
    }

    hstring format_float(float value, wchar_t const* suffix, int precision = 1)
    {
        wchar_t buffer[64]{};
        swprintf_s(buffer, L"%.*f%s", precision, static_cast<double>(value), suffix);
        return hstring{ buffer };
    }

    hstring format_signed_float(float value, wchar_t const* suffix, int precision)
    {
        wchar_t buffer[64]{};
        swprintf_s(buffer, L"%+.*f%s", precision, static_cast<double>(value), suffix);
        return hstring{ buffer };
    }

    hstring format_integer(int64_t value, wchar_t const* suffix)
    {
        wchar_t buffer[64]{};
        swprintf_s(buffer, L"%lld%s", static_cast<long long>(value), suffix);
        return hstring{ buffer };
    }

    hstring format_duration(uint64_t duration_ns)
    {
        uint64_t const seconds = duration_ns / 1000000000ull;
        wchar_t buffer[40]{};
        swprintf_s(buffer,
                   L"%02llu:%02llu:%02llu",
                   static_cast<unsigned long long>(seconds / 3600ull),
                   static_cast<unsigned long long>((seconds / 60ull) % 60ull),
                   static_cast<unsigned long long>(seconds % 60ull));
        return hstring{ buffer };
    }

    hstring local_clock_text()
    {
        SYSTEMTIME now{};
        GetLocalTime(&now);
        wchar_t buffer[24]{};
        swprintf_s(buffer, L"%02u:%02u:%02u", now.wHour, now.wMinute, now.wSecond);
        return hstring{ buffer };
    }

    std::wstring filename_timestamp()
    {
        SYSTEMTIME now{};
        GetLocalTime(&now);
        wchar_t buffer[32]{};
        swprintf_s(buffer,
                   L"%04u%02u%02u_%02u%02u%02u",
                   now.wYear,
                   now.wMonth,
                   now.wDay,
                   now.wHour,
                   now.wMinute,
                   now.wSecond);
        return buffer;
    }

    std::wstring csv_field(std::wstring_view value)
    {
        bool const quote =
            value.find_first_of(L",\"\r\n") != std::wstring_view::npos;
        if (!quote)
        {
            return std::wstring{ value };
        }
        std::wstring escaped;
        escaped.reserve(value.size() + 2u);
        escaped.push_back(L'"');
        for (wchar_t character : value)
        {
            if (character == L'"')
            {
                escaped.push_back(L'"');
            }
            escaped.push_back(character);
        }
        escaped.push_back(L'"');
        return escaped;
    }

    std::wstring csv_float(bool valid, float value, int precision = 3)
    {
        if (!valid || !std::isfinite(value))
        {
            return {};
        }
        wchar_t buffer[64]{};
        swprintf_s(buffer,
                   L"%.*f",
                   precision,
                   static_cast<double>(value));
        return buffer;
    }

    hstring status_text(std::string const& value)
    {
        return winrt::to_hstring(value);
    }

    hstring fic_label_text(char const label[DAB_RECEIVE_CATALOG_LABEL_CAPACITY])
    {
        int length = 0;
        while (length < 16 && label[length] != '\0')
        {
            ++length;
        }
        if (length == 0)
        {
            return {};
        }

        wchar_t wide[32]{};
        int wide_length = MultiByteToWideChar(CP_UTF8,
                                               MB_ERR_INVALID_CHARS,
                                               label,
                                               length,
                                               wide,
                                               static_cast<int>(sizeof(wide) / sizeof(wide[0])));
        if (wide_length == 0)
        {
            wide_length = MultiByteToWideChar(1252,
                                              0,
                                              label,
                                              length,
                                              wide,
                                              static_cast<int>(sizeof(wide) / sizeof(wide[0])));
        }
        return wide_length > 0
            ? hstring{ std::wstring{ wide, static_cast<size_t>(wide_length) } }
            : hstring{};
    }

    hstring component_kind_text(dab_receive_component_kind_t kind)
    {
        switch (kind)
        {
        case DAB_RECEIVE_COMPONENT_STREAM_AUDIO:
            return L"Stream audio";
        case DAB_RECEIVE_COMPONENT_STREAM_DATA:
            return L"Stream data";
        case DAB_RECEIVE_COMPONENT_PACKET_MODE:
            return L"Packet data";
        default:
            return L"Unknown transport";
        }
    }

    hstring msc_protection_text(dab_receive_subchannel_info_t const& subchannel)
    {
        wchar_t text[80]{};
        if (subchannel.protection_kind == 0u)
        {
            swprintf_s(text,
                       L"UEP table %u / index %u",
                       subchannel.protection_table_switch,
                       subchannel.protection_index);
        }
        else
        {
            swprintf_s(text,
                       L"EEP %u-%s",
                       subchannel.protection_level + 1u,
                       subchannel.protection_option == 0u ? L"A" : L"B");
        }
        return hstring{ text };
    }

    hstring bytes_to_hex_lines(uint8_t const* bytes, uint32_t length)
    {
        if (bytes == nullptr || length == 0u)
        {
            return L"—";
        }
        std::wstring result;
        for (uint32_t offset = 0u; offset < length; offset += 16u)
        {
            wchar_t address[16]{};
            swprintf_s(address, L"%04X  ", offset);
            result += address;
            uint32_t const line_end = (std::min)(offset + 16u, length);
            for (uint32_t index = offset; index < line_end; ++index)
            {
                wchar_t byte[4]{};
                swprintf_s(byte, L"%02X ", bytes[index]);
                result += byte;
            }
            if (line_end < length)
            {
                result += L"\r\n";
            }
        }
        return hstring{ result };
    }

    uint64_t file_time_value(FILETIME const& value)
    {
        ULARGE_INTEGER result{};
        result.LowPart = value.dwLowDateTime;
        result.HighPart = value.dwHighDateTime;
        return result.QuadPart;
    }

    uint64_t current_wall_time_100ns()
    {
        FILETIME value{};
        GetSystemTimeAsFileTime(&value);
        return file_time_value(value);
    }

    FILETIME file_time_from_value(uint64_t value)
    {
        ULARGE_INTEGER source{};
        source.QuadPart = value;
        FILETIME result{};
        result.dwLowDateTime = source.LowPart;
        result.dwHighDateTime = source.HighPart;
        return result;
    }

    hstring history_clock_text(uint64_t wall_time_100ns)
    {
        FILETIME const utc = file_time_from_value(wall_time_100ns);
        FILETIME local{};
        SYSTEMTIME time{};
        if (!FileTimeToLocalFileTime(&utc, &local) || !FileTimeToSystemTime(&local, &time))
        {
            return L"--:--:--";
        }
        wchar_t buffer[24]{};
        swprintf_s(buffer, L"%02u:%02u:%02u", time.wHour, time.wMinute, time.wSecond);
        return hstring{ buffer };
    }

    hstring history_clock_ms_text(uint64_t wall_time_100ns)
    {
        FILETIME const utc = file_time_from_value(wall_time_100ns);
        FILETIME local{};
        SYSTEMTIME time{};
        if (!FileTimeToLocalFileTime(&utc, &local) ||
            !FileTimeToSystemTime(&local, &time))
        {
            return L"--:--:--.---";
        }
        wchar_t buffer[32]{};
        swprintf_s(buffer,
                   L"%02u:%02u:%02u.%03u",
                   time.wHour,
                   time.wMinute,
                   time.wSecond,
                   time.wMilliseconds);
        return hstring{ buffer };
    }

    hstring format_scientific(float value)
    {
        wchar_t buffer[40]{};
        swprintf_s(buffer, L"%.2e", static_cast<double>(value));
        return hstring{ buffer };
    }

    hstring bytes_to_hex(uint8_t const* bytes, uint32_t length)
    {
        if (bytes == nullptr || length == 0u)
        {
            return L"—";
        }
        std::wstring result;
        result.reserve(static_cast<size_t>(length) * 3u);
        for (uint32_t index = 0u; index < length; ++index)
        {
            wchar_t byte[4]{};
            swprintf_s(byte, L"%02X", bytes[index]);
            if (!result.empty())
            {
                result.push_back(L' ');
            }
            result += byte;
        }
        return hstring{ result };
    }

    hstring fic_fig_purpose(uint8_t type, uint8_t extension)
    {
        if (type == 0u)
        {
            switch (extension)
            {
            case 0u: return L"Ensemble configuration";
            case 1u: return L"Subchannel organisation";
            case 2u: return L"Service organisation";
            case 3u: return L"Packet-mode component";
            case 5u: return L"Service component language";
            case 8u: return L"Service component definition";
            case 9u: return L"Country / LTO / ECC";
            case 10u: return L"Date and time";
            case 13u: return L"User application information";
            case 17u: return L"Programme type";
            case 18u: return L"Announcement support";
            case 19u: return L"Announcement switching";
            case 21u: return L"Frequency information";
            default: return L"FIG 0 signalling";
            }
        }
        if (type == 1u)
        {
            switch (extension)
            {
            case 0u: return L"Ensemble label";
            case 1u: return L"Service label";
            case 4u: return L"Service component label";
            case 5u: return L"Data service label";
            default: return L"FIG 1 label";
            }
        }
        return L"Unsupported FIG type";
    }

    std::wstring history_iso_utc(uint64_t wall_time_100ns)
    {
        FILETIME const utc = file_time_from_value(wall_time_100ns);
        SYSTEMTIME time{};
        if (!FileTimeToSystemTime(&utc, &time))
        {
            return {};
        }
        wchar_t buffer[40]{};
        swprintf_s(buffer,
                   L"%04u-%02u-%02uT%02u:%02u:%02u.%03uZ",
                   time.wYear,
                   time.wMonth,
                   time.wDay,
                   time.wHour,
                   time.wMinute,
                   time.wSecond,
                   time.wMilliseconds);
        return buffer;
    }

    hstring relative_history_text(uint64_t duration_ns)
    {
        uint64_t const seconds = duration_ns / 1000000000ull;
        wchar_t buffer[40]{};
        if (seconds < 120u)
        {
            swprintf_s(buffer, L"%llu s ago", static_cast<unsigned long long>(seconds));
        }
        else if (seconds < 7200u)
        {
            swprintf_s(buffer, L"%llu min ago",
                       static_cast<unsigned long long>((seconds + 30u) / 60u));
        }
        else
        {
            swprintf_s(buffer, L"%.1f h ago", static_cast<double>(seconds) / 3600.0);
        }
        return hstring{ buffer };
    }

    bool toggle_is_checked(ToggleButton const& button)
    {
        auto const checked = button.IsChecked();
        return checked && checked.Value();
    }

    template <size_t Capacity>
    PointCollection history_points(std::array<float, Capacity> const& values,
                                   size_t write_index,
                                   size_t count,
                                   float minimum,
                                   float maximum,
                                   float width,
                                   float height)
    {
        PointCollection points;
        if (count == 0u || maximum <= minimum)
        {
            return points;
        }

        size_t const first = (write_index + Capacity - count) % Capacity;
        for (size_t point = 0u; point < count; ++point)
        {
            float const value = values[(first + point) % Capacity];
            if (!std::isfinite(value))
            {
                continue;
            }
            float const x = count > 1u
                ? width * static_cast<float>(point) / static_cast<float>(count - 1u)
                : width;
            float const normalized = std::clamp((value - minimum) / (maximum - minimum), 0.0f, 1.0f);
            float const y = (height - 3.0f) - normalized * (height - 6.0f);
            points.Append(Point{ x, y });
        }
        return points;
    }

    template <size_t Capacity>
    PointCollection histogram_points(uint32_t const (&values)[Capacity],
                                     uint32_t maximum,
                                     float width,
                                     float height)
    {
        PointCollection points;
        if (maximum == 0u || Capacity < 2u)
        {
            return points;
        }
        for (size_t bin = 0u; bin < Capacity; ++bin)
        {
            float const x = width * static_cast<float>(bin) /
                            static_cast<float>(Capacity - 1u);
            float const normalized = static_cast<float>(values[bin]) /
                                     static_cast<float>(maximum);
            float const y = (height - 5.0f) - normalized * (height - 10.0f);
            points.Append(Point{ x, y });
        }
        return points;
    }

    template <size_t Capacity>
    bool timed_history_slope(std::array<float, Capacity> const& values,
                             std::array<uint64_t, Capacity> const& timestamps,
                             size_t write_index,
                             size_t count,
                             float& per_minute)
    {
        if (count < 4u)
        {
            return false;
        }

        size_t const first = (write_index + Capacity - count) % Capacity;
        uint64_t origin = 0u;
        double sum_x = 0.0;
        double sum_y = 0.0;
        double sum_xx = 0.0;
        double sum_xy = 0.0;
        uint32_t used = 0u;
        double last_x = 0.0;
        for (size_t point = 0u; point < count; ++point)
        {
            size_t const index = (first + point) % Capacity;
            float const value = values[index];
            uint64_t const timestamp = timestamps[index];
            if (!std::isfinite(value) || timestamp == 0u)
            {
                continue;
            }
            if (origin == 0u)
            {
                origin = timestamp;
            }
            if (timestamp < origin)
            {
                continue;
            }
            double const x = static_cast<double>(timestamp - origin) / 1000000000.0;
            sum_x += x;
            sum_y += value;
            sum_xx += x * x;
            sum_xy += x * value;
            last_x = x;
            ++used;
        }
        double const denominator = static_cast<double>(used) * sum_xx - sum_x * sum_x;
        if (used < 4u || last_x < 2.0 || std::abs(denominator) < 1.0e-9)
        {
            return false;
        }
        double const slope_per_second =
            (static_cast<double>(used) * sum_xy - sum_x * sum_y) / denominator;
        per_minute = static_cast<float>(slope_per_second * 60.0);
        return std::isfinite(per_minute);
    }
}

namespace winrt::DabMeasurementWinUI::implementation
{
    MainWindow::MainWindow()
    {
        m_timer.Interval(std::chrono::milliseconds(250));
        m_timer.Tick({ this, &MainWindow::OnTimerTick });
        m_timer.Start();
    }

    MainWindow::~MainWindow()
    {
        m_timer.Stop();
        m_receiver.StopAndJoin();
        m_session_recorder.Stop();
    }

    void MainWindow::RootLayout_Loaded(
        IInspectable const&,
        RoutedEventArgs const&)
    {
        auto const icon_path = application_icon_path();
        if (!icon_path.empty() &&
            GetFileAttributesW(icon_path.c_str()) != INVALID_FILE_ATTRIBUTES)
        {
            try
            {
                AppWindow().SetIcon(hstring{ icon_path });
            }
            catch (hresult_error const&)
            {
                // The executable still carries the icon resource. A runtime icon
                // assignment failure must never prevent the receiver UI loading.
            }
        }

        m_receiver_menu_ready = true;
        LoadCalibrationProfile();
        UpdateReceiverMenuState();
        UpdateViewMenuState();
        auto const selected_channel =
            ChannelComboBox().SelectedItem().try_as<ComboBoxItem>();
        if (selected_channel)
        {
            double const center_mhz = std::wcstod(
                unbox_value<hstring>(selected_channel.Tag()).c_str(),
                nullptr);
            if (std::isfinite(center_mhz) && center_mhz > 0.0)
            {
                UpdateReceiverSpectrumFrequencyScale(
                    center_mhz * 1000000.0 - 1024000.0,
                center_mhz * 1000000.0 + 1024000.0);
            }
        }
        RefreshRecordingCatalogue();
        UpdateRecordingWorkspace();
    }

    void MainWindow::ResetFileSession(bool closed)
    {
        StopMeasurementRecording(closed
            ? hstring{ L"Recording stopped because the session was closed" }
            : hstring{ L"Recording stopped because a new session was created" });
        CloseReplaySession();
        bool const receiver_active = m_receiver.IsActive();
        m_pending_receiver_restart.reset();
        if (receiver_active)
        {
            m_receiver.RequestStop();
            m_metrics_stale = true;
            StartButton().IsEnabled(false);
            StartButtonLabel().Text(L"Stopping...");
        }
        else
        {
            m_metrics_stale = false;
            SetReceiverControlsEnabled(true);
            StartButton().IsEnabled(true);
            StartButtonLabel().Text(L"Start");
        }

        ClearCatalogView();
        ResetDabOverviewHistory();
        ResetFicMeasurementSession();
        ResetMscMeasurementSession();
        ResetRfSyncDiagnostics();
        ResetRfChannelDiagnostics();
        ResetRfNullScope();
        m_rf_history_write = 0u;
        m_rf_history_count = 0u;
        m_latest_mer_valid = false;
        m_have_latest_rf_snapshot = false;
        m_latest_rf_snapshot = {};
        m_rf_was_locked = false;
        m_rf_lock_started_ns = 0u;
        UpdateRfHistoryVisuals();
        ResetRfHistorySession();
        UpdateCalibrationUi();
        m_fic_mailbox_history_version = m_mailbox_version;

        TimelineList().Items().Clear();
        if (DabEventsList())
        {
            DabEventsList().Items().Clear();
        }
        ShowWorkspace(WorkspaceKind::receiver);
        DataModeText().Text(closed ? L"NO SESSION" : L"NEW SESSION");
        DataSeamText().Text(receiver_active
            ? hstring{ L"waiting for receiver to stop" }
            : (closed
                ? hstring{ L"session closed" }
                : hstring{ L"ready to start receiver" }));
        AddTimelineEvent(closed
            ? hstring{ L"Measurement session closed" }
            : hstring{ L"New measurement session created" });
    }

    void MainWindow::CloseReplaySession()
    {
        bool const was_replay = m_replay_loaded;
        m_session_replay.Close();
        m_replay_loaded = false;
        m_replay_playing = false;
        m_replay_importing = false;
        m_replay_position_ns = 0u;
        m_replay_last_tick = {};
        m_replay_slider_internal = false;
        m_recording_timeline.clear();
        m_recording_events.clear();
        m_recording_markers.clear();
        if (ReplayControlsPanel())
        {
            ReplayControlsPanel().Visibility(Visibility::Collapsed);
        }
        if (was_replay)
        {
            dab_receive_snapshot_t snapshot{};
            uint64_t version = 0u;
            if (m_mailbox.TryReadLatest(m_mailbox_version,
                                        snapshot,
                                        version))
            {
                m_mailbox_version = version;
                m_fic_mailbox_history_version = version;
            }
            dab::measurement::MeasurementFrame measurement;
            if (m_measurement_mailbox.TryReadLatest(
                    m_measurement_mailbox_version,
                    measurement,
                    version))
            {
                m_measurement_mailbox_version = version;
            }
            dab_receive_catalog_t catalog{};
            if (m_catalog_mailbox.TryReadLatest(
                    m_catalog_mailbox_version,
                    catalog,
                    version))
            {
                m_catalog_mailbox_version = version;
            }
        }
        if (RecordingReplayWorkspace())
        {
            UpdateRecordingWorkspace();
        }
    }

    void MainWindow::FileNewSession_Click(
        IInspectable const&,
        RoutedEventArgs const&)
    {
        ResetFileSession(false);
    }

    winrt::fire_and_forget MainWindow::FileOpenRecording_Click(
        IInspectable const&,
        RoutedEventArgs const&)
    {
        auto lifetime = get_strong();
        try
        {
            Windows::Storage::Pickers::FolderPicker picker;
            picker.SuggestedStartLocation(
                Windows::Storage::Pickers::PickerLocationId::DocumentsLibrary);
            picker.FileTypeFilter().Append(L"*");

            HWND window_handle = nullptr;
            check_hresult(
                lifetime.as<::IWindowNative>()->get_WindowHandle(
                    &window_handle));
            check_hresult(
                picker.as<::IInitializeWithWindow>()->Initialize(
                    window_handle));

            auto const folder = co_await picker.PickSingleFolderAsync();
            if (!folder)
            {
                co_return;
            }

            std::filesystem::path const selected_path{
                folder.Path().c_str()
            };
            co_await OpenRecordingPathAsync(selected_path);
        }
        catch (hresult_error const& error)
        {
            m_replay_importing = false;
            AddTimelineEvent(
                L"Recording import failed: " + error.message());
            m_replay_last_tick = std::chrono::steady_clock::now();
            UpdateReceiverMenuState();
        }
        co_return;
    }

    Windows::Foundation::IAsyncAction
    MainWindow::OpenRecordingPathAsync(
        std::filesystem::path selected_path)
    {
        auto lifetime = get_strong();
        m_replay_importing = true;
        DataModeText().Text(L"IMPORTING");
        DataSeamText().Text(L"validating recording and CRCs");
        StartButton().IsEnabled(false);
        RecordButton().IsEnabled(false);
        if (RecordingWorkspaceModeText())
        {
            RecordingWorkspaceModeText().Text(L"IMPORTING");
            RecordingWorkspaceIntegrityText().Text(L"VALIDATING CRC");
        }

        dab::measurement::MeasurementSessionReplay imported;
        std::vector<dab::measurement::MeasurementSessionTimelinePoint>
            imported_timeline;
        std::vector<dab::measurement::MeasurementSessionReplayEvent>
            imported_events;
        std::string import_error;
        auto const dispatcher = DispatcherQueue();
        co_await resume_background();
        bool opened = imported.Open(selected_path, import_error);
        if (opened)
        {
            opened = imported.BuildTimeline(
                2048u,
                imported_timeline,
                imported_events,
                import_error);
        }
        if (!(co_await resume_dispatcher_queue{ dispatcher }))
        {
            co_return;
        }
        m_replay_importing = false;

        if (!opened)
        {
            DataModeText().Text(L"LIVE METRICS");
            DataSeamText().Text(L"recording import failed");
            AddTimelineEvent(
                L"Recording import failed: " +
                to_hstring(import_error));
            m_replay_last_tick = std::chrono::steady_clock::now();
            UpdateRecordingWorkspace();
            UpdateReceiverMenuState();
            co_return;
        }

        StopMeasurementRecording(
            L"Measurement recording stopped to open replay");
        m_pending_receiver_restart.reset();
        m_receiver.StopAndJoin();
        ResetFileSession(false);
        TimelineList().Items().Clear();
        if (DabEventsList())
        {
            DabEventsList().Items().Clear();
        }

        m_session_replay = std::move(imported);
        m_recording_timeline = std::move(imported_timeline);
        m_recording_events = std::move(imported_events);
        m_replay_loaded = true;
        m_replay_playing = true;
        m_replay_position_ns = 0u;
        m_replay_last_tick = std::chrono::steady_clock::now();
        m_metrics_stale = false;
        ReplayControlsPanel().Visibility(Visibility::Visible);

        auto const& info = m_session_replay.Info();
        m_recording_selected_path = info.session_path;
        int32_t channel_index = -1;
        for (uint32_t index = 0u;
             index < ChannelComboBox().Items().Size();
             ++index)
        {
            auto const item =
                ChannelComboBox().Items().GetAt(index)
                    .try_as<ComboBoxItem>();
            if (item &&
                unbox_value<hstring>(item.Content()) ==
                    to_hstring(info.channel))
            {
                channel_index = static_cast<int32_t>(index);
                break;
            }
        }
        if (channel_index >= 0)
        {
            ChannelComboBox().SelectedIndex(channel_index);
        }
        else if (info.frequency_hz != 0u)
        {
            FrequencyTextBox().Text(
                format_float(
                    static_cast<float>(info.frequency_hz) /
                        1000000.0f,
                    L" MHz",
                    3));
        }
        if (!info.device.empty())
        {
            StatusDeviceText().Text(to_hstring(info.device));
        }
        if (!info.channel.empty())
        {
            StatusChannelText().Text(to_hstring(info.channel));
        }

        LoadRecordingMarkers();
        SeekReplay(0u);
        RefreshRecordingCatalogue();
        UpdateRecordingWorkspace();
        AddTimelineEvent(
            L"Recording imported · " +
            hstring{ info.session_path.filename().wstring() } +
            L" · " +
            to_hstring(info.record_count) +
            L" CRC-validated records");
        ShowWorkspace(WorkspaceKind::recording_replay);
        UpdateReplayUi();
        UpdateReceiverMenuState();
        co_return;
    }

    void MainWindow::FileCloseSession_Click(
        IInspectable const&,
        RoutedEventArgs const&)
    {
        ResetFileSession(true);
    }

    winrt::fire_and_forget MainWindow::FileSaveSnapshot_Click(
        IInspectable const&,
        RoutedEventArgs const&)
    {
        auto lifetime = get_strong();
        if (!m_msc_have_snapshot)
        {
            AddTimelineEvent(L"Snapshot not saved — no receiver snapshot is available");
            co_return;
        }
        try
        {
            using namespace Windows::Storage;
            auto const& snapshot = m_msc_latest_snapshot;
            std::wstring text;
            text += L"DAB+ Measurement Workbench snapshot\r\n";
            text += L"Saved: ";
            text += history_iso_utc(current_wall_time_100ns());
            text += L"\r\nContext: ";
            text += DabContextText().Text().c_str();
            text += L"\r\nSnapshot sequence: ";
            text += std::to_wstring(snapshot.sequence);
            text += L"\r\nFrequency: ";
            text += std::to_wstring(snapshot.frequency_hz);
            text += L" Hz\r\nSample rate: ";
            text += std::to_wstring(snapshot.sample_rate_hz);
            text += L" samples/s\r\nLocked: ";
            text += snapshot.locked != 0 ? L"yes" : L"no";
            text += L"\r\nADC RMS: ";
            text += snapshot.adc_level_valid != 0
                ? std::to_wstring(snapshot.adc_level_dbfs) + L" dBFS"
                : L"unavailable";
            text += L"\r\nConnector power: ";
            if (snapshot.calibrated_power_valid != 0)
            {
                text += std::to_wstring(snapshot.calibrated_power_dbm);
                text += L" dBm ±";
                text += std::to_wstring(
                    snapshot.calibrated_power_uncertainty_db);
                text += L" dB";
            }
            else
            {
                text += L"unavailable (";
                text += to_hstring(dab_rf_calibration_status_text(
                    snapshot.calibration_status)).c_str();
                text += L")";
            }
            text += L"\r\nCalibration profile: ";
            text += snapshot.calibration_profile_id[0] != '\0'
                ? to_hstring(snapshot.calibration_profile_id).c_str()
                : L"none";
            text += L"\r\nC/N: ";
            text += snapshot.cn_valid != 0
                ? std::to_wstring(snapshot.cn_db) + L" dB"
                : L"unavailable";
            text += L"\r\nSNR: ";
            text += snapshot.snr_valid != 0
                ? std::to_wstring(snapshot.snr_db) + L" dB"
                : L"unavailable";
            text += L"\r\nCFO: ";
            text += snapshot.cfo_valid != 0
                ? std::to_wstring(snapshot.cfo_hz) + L" Hz"
                : L"unavailable";
            text += L"\r\nFIC valid FIBs: ";
            text += std::to_wstring(snapshot.fic_total_valid_fibs);
            text += L"\r\nMSC BER: ";
            text += snapshot.msc_ber_valid != 0
                ? std::to_wstring(snapshot.msc_ber)
                : L"unavailable";
            text += L"\r\nDAB+ attempts / failures: ";
            text += std::to_wstring(snapshot.msc_decode_attempts);
            text += L" / ";
            text += std::to_wstring(snapshot.msc_decode_failures);
            text += L"\r\nFire-code errors: ";
            text += std::to_wstring(snapshot.msc_firecode_errors_total);
            text += L"\r\nRS corrected bytes / uncorrectable frames: ";
            text += std::to_wstring(snapshot.msc_rs_corrected_total);
            text += L" / ";
            text += std::to_wstring(snapshot.msc_rs_uncorrectable_total);
            text += L"\r\nAU CRC errors / checks: ";
            text += std::to_wstring(snapshot.msc_au_crc_errors_total);
            text += L" / ";
            text += std::to_wstring(snapshot.msc_au_crc_checks_total);
            text += L"\r\nAAC errors: ";
            text += std::to_wstring(snapshot.audio_aac_errors);
            text += L"\r\nAudio availability: ";
            text += std::to_wstring(snapshot.audio_availability_percent);
            text += L"%\r\nReceiver drops / overruns: ";
            text += std::to_wstring(snapshot.dropped_blocks);
            text += L" / ";
            text += std::to_wstring(snapshot.hardware_overruns);
            text += L"\r\n";

            std::wstring const filename =
                L"DAB_Measurement_Snapshot_" +
                filename_timestamp() + L".txt";
            auto const file = co_await DownloadsFolder::CreateFileAsync(
                filename,
                CreationCollisionOption::GenerateUniqueName);
            co_await FileIO::WriteTextAsync(file, hstring{ text });
            AddTimelineEvent(L"Measurement snapshot saved to " + file.Path());
        }
        catch (hresult_error const& error)
        {
            AddTimelineEvent(L"Snapshot save failed: " + error.message());
        }
    }

    winrt::fire_and_forget MainWindow::FileExportMeasurementsCsv_Click(
        IInspectable const&,
        RoutedEventArgs const&)
    {
        auto lifetime = get_strong();
        if (!m_msc_have_snapshot)
        {
            AddTimelineEvent(L"CSV not exported — no receiver snapshot is available");
            co_return;
        }
        try
        {
            using namespace Windows::Storage;
            auto const& s = m_msc_latest_snapshot;
            std::wstring csv =
                L"timestamp_utc,context,sequence,frequency_hz,sample_rate_hz,"
                L"locked,adc_rms_dbfs,adc_peak_dbfs,calibrated_power_dbm,"
                L"calibration_uncertainty_db,calibration_status,"
                L"calibration_profile_id,device_serial,lna_state,"
                L"gain_reduction_db,applied_system_gain_db,cn_db,snr_db,cfo_hz,"
                L"fic_valid_fibs,fic_fig_errors,msc_ber,dabplus_attempts,"
                L"dabplus_failures,firecode_errors,rs_corrected_bytes,"
                L"rs_uncorrectable,au_crc_errors,au_crc_checks,aac_errors,"
                L"audio_availability_percent,audio_clipping_percent,"
                L"dropped_blocks,hardware_overruns\r\n";
            csv += history_iso_utc(current_wall_time_100ns()) + L"," +
                csv_field(DabContextText().Text().c_str()) + L"," +
                std::to_wstring(s.sequence) + L"," +
                std::to_wstring(s.frequency_hz) + L"," +
                std::to_wstring(s.sample_rate_hz) + L"," +
                (s.locked != 0 ? L"1" : L"0") + L"," +
                csv_float(s.adc_level_valid != 0, s.adc_level_dbfs) + L"," +
                csv_float(s.adc_level_valid != 0, s.adc_peak_dbfs) + L"," +
                csv_float(s.calibrated_power_valid != 0,
                          s.calibrated_power_dbm) + L"," +
                csv_float(s.calibrated_power_valid != 0,
                          s.calibrated_power_uncertainty_db) + L"," +
                csv_field(to_hstring(dab_rf_calibration_status_text(
                    s.calibration_status)).c_str()) + L"," +
                csv_field(to_hstring(s.calibration_profile_id).c_str()) + L"," +
                csv_field(to_hstring(s.device_serial).c_str()) + L"," +
                std::to_wstring(s.lna_state) + L"," +
                std::to_wstring(s.if_gain_reduction_db) + L"," +
                csv_float(s.applied_system_gain_valid != 0,
                          s.applied_system_gain_db) + L"," +
                csv_float(s.cn_valid != 0, s.cn_db) + L"," +
                csv_float(s.snr_valid != 0, s.snr_db) + L"," +
                csv_float(s.cfo_valid != 0, s.cfo_hz) + L"," +
                std::to_wstring(s.fic_total_valid_fibs) + L"," +
                std::to_wstring(s.fic_total_fig_parse_errors) + L"," +
                csv_float(s.msc_ber_valid != 0, s.msc_ber, 9) + L"," +
                std::to_wstring(s.msc_decode_attempts) + L"," +
                std::to_wstring(s.msc_decode_failures) + L"," +
                std::to_wstring(s.msc_firecode_errors_total) + L"," +
                std::to_wstring(s.msc_rs_corrected_total) + L"," +
                std::to_wstring(s.msc_rs_uncorrectable_total) + L"," +
                std::to_wstring(s.msc_au_crc_errors_total) + L"," +
                std::to_wstring(s.msc_au_crc_checks_total) + L"," +
                std::to_wstring(s.audio_aac_errors) + L"," +
                csv_float(s.audio_output_frames != 0u,
                          s.audio_availability_percent) + L"," +
                csv_float(s.audio_total_samples != 0u,
                          s.audio_clipping_percent,
                          6) + L"," +
                std::to_wstring(s.dropped_blocks) + L"," +
                (s.hardware_overruns_valid != 0
                    ? std::to_wstring(s.hardware_overruns)
                    : std::wstring{}) + L"\r\n";

            std::wstring const filename =
                L"DAB_Current_Measurements_" +
                filename_timestamp() + L".csv";
            auto const file = co_await DownloadsFolder::CreateFileAsync(
                filename,
                CreationCollisionOption::GenerateUniqueName);
            co_await FileIO::WriteTextAsync(file, hstring{ csv });
            AddTimelineEvent(L"Current measurements CSV saved to " + file.Path());
        }
        catch (hresult_error const& error)
        {
            AddTimelineEvent(L"Measurement CSV export failed: " + error.message());
        }
    }

    winrt::fire_and_forget MainWindow::FileExportEventsCsv_Click(
        IInspectable const&,
        RoutedEventArgs const&)
    {
        auto lifetime = get_strong();
        try
        {
            using namespace Windows::Storage;
            std::wstring csv =
                L"source,timestamp_utc,severity,measurement,value,description\r\n";
            if (DabEventsList())
            {
                auto const items = DabEventsList().Items();
                for (uint32_t index = 0u; index < items.Size(); ++index)
                {
                    hstring const event_text =
                        unbox_value_or<hstring>(items.GetAt(index), L"");
                    csv += L"timeline,,Info,,," +
                        csv_field(event_text.c_str()) + L"\r\n";
                }
            }
            for (auto const& event : m_rf_history_events)
            {
                csv += L"rf," +
                    history_iso_utc(event.wall_time_100ns) + L"," +
                    csv_field(event.severity) + L"," +
                    csv_field(event.measurement) + L"," +
                    csv_field(event.value) + L"," +
                    csv_field(event.description) + L"\r\n";
            }
            if (DabPlusAlarmList())
            {
                auto const alarms = DabPlusAlarmList().Items();
                for (uint32_t index = 0u; index < alarms.Size(); ++index)
                {
                    hstring const alarm =
                        unbox_value_or<hstring>(alarms.GetAt(index), L"");
                    if (alarm.starts_with(L"No current"))
                    {
                        continue;
                    }
                    csv += L"dabplus," +
                        history_iso_utc(current_wall_time_100ns()) +
                        L",Warning,DAB+,,"
                        + csv_field(alarm.c_str()) + L"\r\n";
                }
            }

            std::wstring const filename =
                L"DAB_Events_Alarms_" +
                filename_timestamp() + L".csv";
            auto const file = co_await DownloadsFolder::CreateFileAsync(
                filename,
                CreationCollisionOption::GenerateUniqueName);
            co_await FileIO::WriteTextAsync(file, hstring{ csv });
            AddTimelineEvent(L"Events and alarms CSV saved to " + file.Path());
        }
        catch (hresult_error const& error)
        {
            AddTimelineEvent(L"Events CSV export failed: " + error.message());
        }
    }

    winrt::fire_and_forget MainWindow::FileSaveScreenshot_Click(
        IInspectable const&,
        RoutedEventArgs const&)
    {
        auto lifetime = get_strong();
        try
        {
            using namespace Windows::Graphics::Imaging;
            using namespace Windows::Storage;
            using namespace Windows::Storage::Streams;
            using namespace Microsoft::UI::Xaml::Media::Imaging;

            FrameworkElement capture = ReceiverWorkspaceMain();
            std::wstring workspace_name = L"Receiver";
            if (m_active_workspace == WorkspaceKind::rf_measurements)
            {
                capture = RfWorkspace();
                workspace_name = L"RF";
            }
            else if (m_active_workspace == WorkspaceKind::dab_measurements)
            {
                capture = DabWorkspace();
                workspace_name = L"DAB";
            }

            RenderTargetBitmap bitmap;
            co_await bitmap.RenderAsync(capture);
            auto const pixel_buffer = co_await bitmap.GetPixelsAsync();
            DataReader reader = DataReader::FromBuffer(pixel_buffer);
            std::vector<uint8_t> pixels(pixel_buffer.Length());
            reader.ReadBytes(pixels);

            std::wstring const filename =
                L"DAB_" + workspace_name + L"_Workspace_" +
                filename_timestamp() + L".png";
            auto const file = co_await KnownFolders::PicturesLibrary().CreateFileAsync(
                filename,
                CreationCollisionOption::GenerateUniqueName);
            auto const stream = co_await file.OpenAsync(FileAccessMode::ReadWrite);
            auto const encoder = co_await BitmapEncoder::CreateAsync(
                BitmapEncoder::PngEncoderId(),
                stream);
            encoder.SetPixelData(BitmapPixelFormat::Bgra8,
                                 BitmapAlphaMode::Premultiplied,
                                 static_cast<uint32_t>(bitmap.PixelWidth()),
                                 static_cast<uint32_t>(bitmap.PixelHeight()),
                                 96.0,
                                 96.0,
                                 pixels);
            co_await encoder.FlushAsync();
            AddTimelineEvent(L"Workspace screenshot saved to " + file.Path());
        }
        catch (hresult_error const& error)
        {
            AddTimelineEvent(L"Workspace screenshot failed: " + error.message());
        }
    }

    void MainWindow::FileExit_Click(
        IInspectable const&,
        RoutedEventArgs const&)
    {
        Close();
    }

    void MainWindow::ReceiverStartStop_Click(
        IInspectable const& sender,
        RoutedEventArgs const& args)
    {
        if (!StartButton().IsEnabled())
        {
            return;
        }
        StartButton_Click(sender, args);
        UpdateReceiverMenuState();
    }

    void MainWindow::ReceiverDevice_Click(
        IInspectable const& sender,
        RoutedEventArgs const&)
    {
        if (m_receiver.IsActive())
        {
            UpdateReceiverMenuState();
            return;
        }

        auto const item = sender.try_as<ToggleMenuFlyoutItem>();
        if (!item || !item.Tag())
        {
            return;
        }
        std::wstring const value{ unbox_value<hstring>(item.Tag()).c_str() };
        wchar_t* end = nullptr;
        long const index = std::wcstol(value.c_str(), &end, 10);
        if (end == value.c_str() || index < 0 ||
            index >= static_cast<long>(DeviceComboBox().Items().Size()))
        {
            UpdateReceiverMenuState();
            return;
        }

        DeviceComboBox().SelectedIndex(static_cast<int32_t>(index));
        UpdateReceiverMenuState();
        AddTimelineEvent(index == 1
            ? hstring{ L"Receiver device set to RTL-SDR" }
            : hstring{ L"Receiver device set to SDRplay RSP1A" });
    }

    void MainWindow::SelectReceiverChannel(int32_t index)
    {
        if (m_receiver.IsActive() ||
            index < 0 ||
            index >= static_cast<int32_t>(ChannelComboBox().Items().Size()))
        {
            return;
        }

        if (ChannelComboBox().SelectedIndex() != index)
        {
            ChannelComboBox().SelectedIndex(index);
            auto const selected = ChannelComboBox().SelectedItem().try_as<ComboBoxItem>();
            if (selected)
            {
                AddTimelineEvent(
                    L"Tuned to Band III channel " +
                    unbox_value<hstring>(selected.Content()));
            }
        }
        UpdateReceiverMenuState();
    }

    void MainWindow::ReceiverPreviousChannel_Click(
        IInspectable const&,
        RoutedEventArgs const&)
    {
        SelectReceiverChannel(ChannelComboBox().SelectedIndex() - 1);
    }

    void MainWindow::ReceiverNextChannel_Click(
        IInspectable const&,
        RoutedEventArgs const&)
    {
        SelectReceiverChannel(ChannelComboBox().SelectedIndex() + 1);
    }

    void MainWindow::ReceiverChannel_Click(
        IInspectable const& sender,
        RoutedEventArgs const&)
    {
        auto const item = sender.try_as<MenuFlyoutItem>();
        if (!item || !item.Tag())
        {
            return;
        }
        std::wstring const value{ unbox_value<hstring>(item.Tag()).c_str() };
        wchar_t* end = nullptr;
        long const index = std::wcstol(value.c_str(), &end, 10);
        if (end != value.c_str())
        {
            SelectReceiverChannel(static_cast<int32_t>(index));
        }
    }

    void MainWindow::ReceiverEnterFrequency_Click(
        IInspectable const&,
        RoutedEventArgs const&)
    {
        if (m_receiver.IsActive())
        {
            return;
        }
        ShowWorkspace(WorkspaceKind::receiver);
        FrequencyTextBox().Focus(FocusState::Programmatic);
        FrequencyTextBox().SelectAll();
    }

    void MainWindow::ReceiverGainAdjust_Click(
        IInspectable const& sender,
        RoutedEventArgs const&)
    {
        if (m_receiver.IsActive() || m_receiver_agc_enabled)
        {
            return;
        }
        auto const item = sender.try_as<MenuFlyoutItem>();
        if (!item || !item.Tag())
        {
            return;
        }
        std::wstring const value{ unbox_value<hstring>(item.Tag()).c_str() };
        wchar_t* end = nullptr;
        long const delta = std::wcstol(value.c_str(), &end, 10);
        if (end == value.c_str())
        {
            return;
        }
        double gain = GainNumberBox().Value();
        if (!std::isfinite(gain))
        {
            gain = 40.0;
        }
        gain = std::clamp(gain + static_cast<double>(delta), 0.0, 100.0);
        GainNumberBox().Value(gain);
        UpdateReceiverMenuState();
    }

    void MainWindow::ReceiverGainPreset_Click(
        IInspectable const& sender,
        RoutedEventArgs const&)
    {
        if (m_receiver.IsActive() || m_receiver_agc_enabled)
        {
            return;
        }
        auto const item = sender.try_as<MenuFlyoutItem>();
        if (!item || !item.Tag())
        {
            return;
        }
        std::wstring const value{ unbox_value<hstring>(item.Tag()).c_str() };
        wchar_t* end = nullptr;
        long const gain = std::wcstol(value.c_str(), &end, 10);
        if (end == value.c_str())
        {
            return;
        }
        GainNumberBox().Value(static_cast<double>(std::clamp(gain, 0l, 100l)));
        UpdateReceiverMenuState();
    }

    void MainWindow::ReceiverAgc_Click(
        IInspectable const&,
        RoutedEventArgs const&)
    {
        if (m_receiver.IsActive())
        {
            UpdateReceiverMenuState();
            return;
        }
        m_receiver_agc_enabled = ReceiverAgcMenuItem().IsChecked();
        SetReceiverControlsEnabled(true);
        UpdateReceiverMenuState();
        AddTimelineEvent(m_receiver_agc_enabled
            ? hstring{ L"Receiver AGC enabled" }
            : hstring{ L"Receiver AGC disabled" });
    }

    void MainWindow::ReceiverPpmAdjust_Click(
        IInspectable const& sender,
        RoutedEventArgs const&)
    {
        if (m_receiver.IsActive())
        {
            return;
        }
        auto const item = sender.try_as<MenuFlyoutItem>();
        if (!item || !item.Tag())
        {
            return;
        }
        std::wstring const value{ unbox_value<hstring>(item.Tag()).c_str() };
        if (value == L"reset")
        {
            m_receiver_ppm_correction = 0;
        }
        else
        {
            wchar_t* end = nullptr;
            long const delta = std::wcstol(value.c_str(), &end, 10);
            if (end == value.c_str())
            {
                return;
            }
            m_receiver_ppm_correction = std::clamp(
                m_receiver_ppm_correction + static_cast<int32_t>(delta),
                -100,
                100);
        }
        UpdateReceiverMenuState();
    }

    void MainWindow::ReceiverBiasTee_Click(
        IInspectable const&,
        RoutedEventArgs const&)
    {
        if (m_receiver.IsActive())
        {
            UpdateReceiverMenuState();
            return;
        }
        m_receiver_bias_tee_enabled = ReceiverBiasTeeMenuItem().IsChecked();
        UpdateReceiverMenuState();
        AddTimelineEvent(m_receiver_bias_tee_enabled
            ? hstring{ L"Receiver bias tee enabled" }
            : hstring{ L"Receiver bias tee disabled" });
    }

    void MainWindow::ReceiverOpenServiceBrowser_Click(
        IInspectable const&,
        RoutedEventArgs const&)
    {
        ShowWorkspace(WorkspaceKind::receiver);
        ServicesListView().Focus(FocusState::Programmatic);
    }

    void MainWindow::ReceiverDiagnostics_Click(
        IInspectable const&,
        RoutedEventArgs const&)
    {
        ShowWorkspace(WorkspaceKind::rf_measurements);
        RfTabView().SelectedIndex(2);
    }

    void MainWindow::ChannelComboBox_SelectionChanged(IInspectable const&, SelectionChangedEventArgs const&)
    {
        auto selected_channel = ChannelComboBox().SelectedItem().try_as<ComboBoxItem>();
        if (!selected_channel)
        {
            return;
        }

        auto const channel = unbox_value<hstring>(selected_channel.Content());
        auto const frequency = unbox_value<hstring>(selected_channel.Tag()) + L" MHz";
        if (auto frequency_box = FrequencyTextBox())
        {
            frequency_box.Text(frequency);
        }
        if (auto spectrum_frequency = SpectrumFrequencyText())
        {
            spectrum_frequency.Text(frequency);
        }
        if (auto status_channel = StatusChannelText())
        {
            status_channel.Text(channel);
        }
        if (m_receiver_menu_ready)
        {
            double const center_mhz = std::wcstod(
                unbox_value<hstring>(selected_channel.Tag()).c_str(),
                nullptr);
            if (std::isfinite(center_mhz) && center_mhz > 0.0)
            {
                UpdateReceiverSpectrumFrequencyScale(
                    center_mhz * 1000000.0 - 1024000.0,
                    center_mhz * 1000000.0 + 1024000.0);
            }
        }
        m_selected_service_id = 0u;
        UpdateRfContext();
        UpdateDabContext();
        UpdateReceiverMenuState();
    }

    void MainWindow::ReceiverWorkspace_Click(IInspectable const&, RoutedEventArgs const&)
    {
        ShowWorkspace(WorkspaceKind::receiver);
    }

    void MainWindow::RfMeasurements_Click(IInspectable const&, RoutedEventArgs const&)
    {
        ShowWorkspace(WorkspaceKind::rf_measurements);
    }

    void MainWindow::DabMeasurements_Click(IInspectable const&, RoutedEventArgs const&)
    {
        ShowWorkspace(WorkspaceKind::dab_measurements);
    }

    void MainWindow::RecordingReplay_Click(
        IInspectable const&,
        RoutedEventArgs const&)
    {
        ShowWorkspace(WorkspaceKind::recording_replay);
        RefreshRecordingCatalogue();
        UpdateRecordingWorkspace();
    }

    void MainWindow::RecordingRefresh_Click(
        IInspectable const&,
        RoutedEventArgs const&)
    {
        RefreshRecordingCatalogue();
        RefreshRecordingCaptures();
    }

    void MainWindow::RefreshRecordingCatalogue()
    {
        if (!RecordingSessionListView())
        {
            return;
        }

        auto const previous_selection = m_recording_selected_path;
        m_recording_catalog.clear();
        RecordingSessionListView().Items().Clear();
        RecordingCompareSessionComboBox().Items().Clear();

        auto const root = measurement_recordings_root();
        std::error_code error;
        if (std::filesystem::exists(root, error))
        {
            std::filesystem::directory_iterator iterator{ root, error };
            std::filesystem::directory_iterator const end;
            while (!error && iterator != end)
            {
                auto const entry = *iterator;
                iterator.increment(error);
                if (!entry.is_directory())
                {
                    continue;
                }

                dab::measurement::MeasurementSessionReplayInfo info;
                std::string read_error;
                if (dab::measurement::MeasurementSessionReplay::ReadSessionInfo(
                        entry.path(),
                        info,
                        read_error))
                {
                    m_recording_catalog.push_back(std::move(info));
                }
            }
        }

        std::sort(
            m_recording_catalog.begin(),
            m_recording_catalog.end(),
            [](auto const& left, auto const& right)
            {
                if (left.started_unix_ns != right.started_unix_ns)
                {
                    return left.started_unix_ns > right.started_unix_ns;
                }
                return left.session_path.filename().wstring() >
                    right.session_path.filename().wstring();
            });

        int32_t selected_index = -1;
        int32_t active_index = -1;
        for (size_t index = 0u; index < m_recording_catalog.size(); ++index)
        {
            auto const& info = m_recording_catalog[index];
            StackPanel panel;
            panel.Spacing(2.0);

            TextBlock name;
            name.Text(hstring{ info.session_path.filename().wstring() });
            name.FontSize(15.0);
            panel.Children().Append(name);

            TextBlock context;
            context.Text(
                to_hstring(info.channel.empty() ? "No channel" : info.channel) +
                L" · " +
                to_hstring(info.ensemble.empty() ? "No ensemble" : info.ensemble) +
                L" · " +
                to_hstring(info.service.empty() ? "No service" : info.service));
            context.Foreground(
                Application::Current().Resources().Lookup(
                    box_value(hstring{ L"MutedTextBrush" })).as<Brush>());
            context.FontSize(11.0);
            context.TextTrimming(TextTrimming::CharacterEllipsis);
            panel.Children().Append(context);

            TextBlock detail;
            wchar_t detail_buffer[160]{};
            swprintf_s(
                detail_buffer,
                L"%ls · %llu records · %.1f MB · %ls",
                format_duration(info.duration_ns).c_str(),
                static_cast<unsigned long long>(info.record_count),
                static_cast<double>(info.bytes_written) / (1024.0 * 1024.0),
                to_hstring(info.state.empty() ? "unknown" : info.state).c_str());
            detail.Text(detail_buffer);
            detail.Foreground(
                Application::Current().Resources().Lookup(
                    box_value(hstring{ L"MutedTextBrush" })).as<Brush>());
            detail.FontSize(11.0);
            panel.Children().Append(detail);

            ListViewItem row;
            row.Content(panel);
            row.Tag(box_value(hstring{ info.session_path.wstring() }));
            row.HorizontalContentAlignment(HorizontalAlignment::Stretch);
            RecordingSessionListView().Items().Append(row);

            if (!previous_selection.empty() &&
                info.session_path.lexically_normal() ==
                    previous_selection.lexically_normal())
            {
                selected_index = static_cast<int32_t>(index);
            }
            if (m_replay_loaded &&
                info.session_path.lexically_normal() ==
                    m_session_replay.Info().session_path.lexically_normal())
            {
                active_index = static_cast<int32_t>(index);
            }
        }

        if (selected_index < 0)
        {
            selected_index = active_index >= 0
                ? active_index
                : (m_recording_catalog.empty() ? -1 : 0);
        }
        if (selected_index >= 0)
        {
            RecordingSessionListView().SelectedIndex(selected_index);
            m_recording_selected_path =
                m_recording_catalog[static_cast<size_t>(selected_index)]
                    .session_path;
        }
        else
        {
            m_recording_selected_path.clear();
        }

        for (auto const& info : m_recording_catalog)
        {
            if (m_replay_loaded &&
                info.session_path.lexically_normal() ==
                    m_session_replay.Info().session_path.lexically_normal())
            {
                continue;
            }
            ComboBoxItem item;
            item.Content(box_value(
                hstring{ info.session_path.filename().wstring() } +
                L" · " +
                to_hstring(info.channel.empty() ? "—" : info.channel)));
            item.Tag(box_value(hstring{ info.session_path.wstring() }));
            RecordingCompareSessionComboBox().Items().Append(item);
        }

        RecordingCatalogueCountText().Text(
            to_hstring(m_recording_catalog.size()) +
            (m_recording_catalog.size() == 1u
                ? L" session"
                : L" sessions"));
        UpdateRecordingSelectedSession();
    }

    void MainWindow::RecordingSessionList_SelectionChanged(
        IInspectable const&,
        SelectionChangedEventArgs const&)
    {
        auto const item =
            RecordingSessionListView().SelectedItem().try_as<ListViewItem>();
        if (item && item.Tag())
        {
            m_recording_selected_path =
                std::filesystem::path{
                    unbox_value<hstring>(item.Tag()).c_str()
                };
        }
        UpdateRecordingSelectedSession();
    }

    void MainWindow::UpdateRecordingSelectedSession()
    {
        if (!RecordingSelectedNameText())
        {
            return;
        }

        auto selected = std::find_if(
            m_recording_catalog.begin(),
            m_recording_catalog.end(),
            [&](auto const& info)
            {
                return info.session_path.lexically_normal() ==
                    m_recording_selected_path.lexically_normal();
            });
        if (selected == m_recording_catalog.end())
        {
            RecordingSelectedNameText().Text(L"Select a recording");
            RecordingSelectedContextText().Text(
                L"Channel, ensemble and service will appear here");
            RecordingSelectedIntegrityBar().Severity(
                InfoBarSeverity::Informational);
            RecordingSelectedIntegrityBar().Title(L"Not validated");
            RecordingSelectedIntegrityBar().Message(
                L"Open a recording to validate its chunks and record CRCs.");
            RecordingSelectedDurationText().Text(L"--:--:--");
            RecordingSelectedRecordsText().Text(L"0");
            RecordingSelectedSizeText().Text(L"0 MB");
            RecordingSelectedDroppedText().Text(L"0");
            RecordingSelectedDeviceText().Text(L"-");
            RecordingSelectedSampleRateText().Text(L"-");
            RecordingSelectedSnapshotsText().Text(L"0");
            RecordingSelectedMeasurementsText().Text(L"0");
            RecordingSelectedCatalogsText().Text(L"0");
            RecordingSelectedEventsText().Text(L"0");
            RecordingSelectedChunksText().Text(L"0");
            RecordingSelectedStateText().Text(L"-");
            return;
        }

        auto const& info = *selected;
        RecordingSelectedNameText().Text(
            hstring{ info.session_path.filename().wstring() });
        wchar_t frequency[48]{};
        swprintf_s(
            frequency,
            L"%.3f MHz",
            static_cast<double>(info.frequency_hz) / 1000000.0);
        RecordingSelectedContextText().Text(
            to_hstring(info.channel.empty() ? "—" : info.channel) +
            L" · " +
            frequency +
            L" · " +
            to_hstring(info.ensemble.empty() ? "No ensemble" : info.ensemble) +
            L" · " +
            to_hstring(info.service.empty() ? "No service" : info.service));

        bool const validated =
            m_replay_loaded &&
            info.session_path.lexically_normal() ==
                m_session_replay.Info().session_path.lexically_normal();
        if (validated)
        {
            RecordingSelectedIntegrityBar().Severity(
                info.dropped_records == 0u
                    ? InfoBarSeverity::Success
                    : InfoBarSeverity::Warning);
            RecordingSelectedIntegrityBar().Title(
                info.dropped_records == 0u
                    ? L"CRC validated"
                    : L"Validated with recorder drops");
            RecordingSelectedIntegrityBar().Message(
                to_hstring(info.record_count) +
                L" records validated across " +
                to_hstring(info.chunk_count) +
                L" chunk files.");
        }
        else
        {
            RecordingSelectedIntegrityBar().Severity(
                info.state == "complete"
                    ? InfoBarSeverity::Informational
                    : InfoBarSeverity::Warning);
            RecordingSelectedIntegrityBar().Title(
                info.state == "complete"
                    ? L"Manifest inspected"
                    : L"Incomplete recording");
            RecordingSelectedIntegrityBar().Message(
                L"Open this recording to validate every chunk and record CRC.");
        }

        RecordingSelectedDurationText().Text(format_duration(info.duration_ns));
        RecordingSelectedRecordsText().Text(to_hstring(info.record_count));
        RecordingSelectedSizeText().Text(
            format_float(
                static_cast<float>(
                    static_cast<double>(info.bytes_written) /
                    (1024.0 * 1024.0)),
                L" MB",
                1));
        RecordingSelectedDroppedText().Text(to_hstring(info.dropped_records));
        RecordingSelectedDeviceText().Text(
            to_hstring(info.device.empty() ? "Unknown" : info.device));
        RecordingSelectedSampleRateText().Text(
            info.sample_rate_hz == 0u
                ? hstring{ L"-" }
                : format_float(
                    static_cast<float>(info.sample_rate_hz) / 1000000.0f,
                    L" MS/s",
                    3));
        RecordingSelectedSnapshotsText().Text(
            to_hstring(info.snapshot_records));
        RecordingSelectedMeasurementsText().Text(
            to_hstring(info.measurement_records));
        RecordingSelectedCatalogsText().Text(
            to_hstring(info.catalog_records));
        RecordingSelectedEventsText().Text(to_hstring(info.event_records));
        RecordingSelectedChunksText().Text(to_hstring(info.chunk_count));
        RecordingSelectedStateText().Text(
            to_hstring(info.state.empty() ? "unknown" : info.state));
    }

    winrt::fire_and_forget MainWindow::RecordingOpenSelected_Click(
        IInspectable const&,
        RoutedEventArgs const&)
    {
        auto lifetime = get_strong();
        if (m_recording_selected_path.empty())
        {
            AddTimelineEvent(L"Select a recording to open");
            co_return;
        }
        try
        {
            co_await OpenRecordingPathAsync(m_recording_selected_path);
        }
        catch (hresult_error const& error)
        {
            m_replay_importing = false;
            AddTimelineEvent(
                L"Recording import failed: " + error.message());
            UpdateRecordingWorkspace();
            UpdateReceiverMenuState();
        }
    }

    winrt::fire_and_forget MainWindow::RecordingOpenFolder_Click(
        IInspectable const&,
        RoutedEventArgs const&)
    {
        auto lifetime = get_strong();
        std::filesystem::path folder_path =
            m_replay_loaded
                ? m_session_replay.Info().session_path
                : m_recording_selected_path;
        if (folder_path.empty())
        {
            folder_path = measurement_recordings_root();
        }

        std::error_code error;
        if (folder_path == measurement_recordings_root())
        {
            std::filesystem::create_directories(folder_path, error);
        }
        if (error || !std::filesystem::is_directory(folder_path))
        {
            AddTimelineEvent(L"Recording folder is unavailable");
            co_return;
        }

        try
        {
            auto const folder =
                co_await Windows::Storage::StorageFolder::GetFolderFromPathAsync(
                    folder_path.wstring());
            bool const opened =
                co_await Windows::System::Launcher::LaunchFolderAsync(folder);
            AddTimelineEvent(
                opened
                    ? hstring{ L"Opened recording folder" }
                    : hstring{ L"Windows declined to open the recording folder" });
        }
        catch (hresult_error const& error)
        {
            AddTimelineEvent(
                L"Unable to open recording folder: " + error.message());
        }
    }

    void MainWindow::RecordingTimelineCanvas_PointerPressed(
        IInspectable const&,
        PointerRoutedEventArgs const& args)
    {
        if (!m_replay_loaded)
        {
            return;
        }
        double const width = RecordingTimelineCanvas().ActualWidth();
        if (!(width > 0.0))
        {
            return;
        }
        double const x = std::clamp(
            static_cast<double>(
                args.GetCurrentPoint(
                    RecordingTimelineCanvas()).Position().X),
            0.0,
            width);
        uint64_t const duration = m_session_replay.Info().duration_ns;
        uint64_t const target = static_cast<uint64_t>(
            static_cast<double>(duration) * x / width);
        m_replay_playing = false;
        SeekReplay(target);
        UpdateReplayUi();
    }

    void MainWindow::RenderRecordingTimeline()
    {
        if (!RecordingTimelineCanvas())
        {
            return;
        }

        PointCollection power_points;
        PointCollection mer_points;
        PointCollection cfo_points;
        PointCollection ber_points;
        PointCollection lock_points;
        if (m_recording_timeline.empty())
        {
            RecordingPowerTrace().Points(power_points);
            RecordingMerTrace().Points(mer_points);
            RecordingCfoTrace().Points(cfo_points);
            RecordingBerTrace().Points(ber_points);
            RecordingLockTrace().Points(lock_points);
            RecordingPowerScaleText().Text(L"Power: -");
            RecordingMerScaleText().Text(L"MER: -");
            RecordingCfoScaleText().Text(L"CFO: -");
            UpdateRecordingTimelineCursor();
            return;
        }

        auto value_range =
            [&](auto value, auto valid, float fallback_low, float fallback_high)
            {
                float low = (std::numeric_limits<float>::max)();
                float high = (std::numeric_limits<float>::lowest)();
                for (auto const& point : m_recording_timeline)
                {
                    if (valid(point) && std::isfinite(value(point)))
                    {
                        low = (std::min)(low, value(point));
                        high = (std::max)(high, value(point));
                    }
                }
                if (low > high)
                {
                    return std::pair<float, float>{ fallback_low, fallback_high };
                }
                if (high - low < 1.0f)
                {
                    low -= 0.5f;
                    high += 0.5f;
                }
                return std::pair<float, float>{ low, high };
            };
        auto const power_range = value_range(
            [](auto const& point) { return point.power; },
            [](auto const& point) { return point.power_valid; },
            -100.0f,
            0.0f);
        auto const mer_range = value_range(
            [](auto const& point) { return point.mer_db; },
            [](auto const& point) { return point.mer_valid; },
            0.0f,
            30.0f);
        auto const cfo_range = value_range(
            [](auto const& point) { return point.cfo_hz; },
            [](auto const& point) { return point.cfo_valid; },
            -10.0f,
            10.0f);
        float const cfo_extent = (std::max)(
            10.0f,
            (std::max)(std::abs(cfo_range.first), std::abs(cfo_range.second)));
        uint64_t const duration = (std::max)(
            m_session_replay.Info().duration_ns,
            m_recording_timeline.back().timestamp_ns);

        auto const x_for = [&](uint64_t timestamp_ns)
        {
            return duration == 0u
                ? 0.0
                : 1000.0 *
                    static_cast<double>(timestamp_ns) /
                    static_cast<double>(duration);
        };
        auto const lane_y =
            [](float value, float low, float high, double top)
            {
                double const fraction = std::clamp(
                    static_cast<double>(value - low) /
                        static_cast<double>(high - low),
                    0.0,
                    1.0);
                return top + 46.0 * (1.0 - fraction);
            };

        for (auto const& point : m_recording_timeline)
        {
            double const x = x_for(point.timestamp_ns);
            if (point.power_valid)
            {
                power_points.Append(
                    Point{
                        static_cast<float>(x),
                        static_cast<float>(lane_y(
                            point.power,
                            power_range.first,
                            power_range.second,
                            8.0))
                    });
            }
            if (point.mer_valid)
            {
                mer_points.Append(
                    Point{
                        static_cast<float>(x),
                        static_cast<float>(lane_y(
                            point.mer_db,
                            mer_range.first,
                            mer_range.second,
                            64.0))
                    });
            }
            if (point.cfo_valid)
            {
                cfo_points.Append(
                    Point{
                        static_cast<float>(x),
                        static_cast<float>(lane_y(
                            point.cfo_hz,
                            -cfo_extent,
                            cfo_extent,
                            120.0))
                    });
            }
            if (point.msc_ber_valid)
            {
                float const quality = static_cast<float>(std::clamp(
                    -std::log10((std::max)(point.msc_ber, 1.0e-8f)) / 8.0,
                    0.0,
                    1.0));
                ber_points.Append(
                    Point{
                        static_cast<float>(x),
                        static_cast<float>(lane_y(
                            quality,
                            0.0f,
                            1.0f,
                            176.0))
                    });
            }
            lock_points.Append(
                Point{
                    static_cast<float>(x),
                    point.locked ? 248.0f : 288.0f
                });
        }

        RecordingPowerTrace().Points(power_points);
        RecordingMerTrace().Points(mer_points);
        RecordingCfoTrace().Points(cfo_points);
        RecordingBerTrace().Points(ber_points);
        RecordingLockTrace().Points(lock_points);

        bool any_calibrated = false;
        bool any_uncalibrated = false;
        for (auto const& point : m_recording_timeline)
        {
            if (!point.power_valid)
            {
                continue;
            }
            any_calibrated |= point.power_calibrated;
            any_uncalibrated |= !point.power_calibrated;
        }
        wchar_t scale[160]{};
        swprintf_s(
            scale,
            L"Power: %.1f…%.1f %ls",
            static_cast<double>(power_range.first),
            static_cast<double>(power_range.second),
            any_calibrated && !any_uncalibrated ? L"dBm" :
            (!any_calibrated ? L"dBFS" : L"dBm/dBFS"));
        RecordingPowerScaleText().Text(scale);
        swprintf_s(
            scale,
            L"MER: %.1f…%.1f dB",
            static_cast<double>(mer_range.first),
            static_cast<double>(mer_range.second));
        RecordingMerScaleText().Text(scale);
        swprintf_s(
            scale,
            L"CFO: ±%.1f Hz",
            static_cast<double>(cfo_extent));
        RecordingCfoScaleText().Text(scale);
        UpdateRecordingTimelineCursor();
    }

    void MainWindow::UpdateRecordingTimelineCursor()
    {
        if (!RecordingTimelineCursor())
        {
            return;
        }
        uint64_t const duration =
            m_replay_loaded ? m_session_replay.Info().duration_ns : 0u;
        double const x = duration == 0u
            ? 0.0
            : 1000.0 *
                static_cast<double>((std::min)(m_replay_position_ns, duration)) /
                static_cast<double>(duration);
        RecordingTimelineCursor().X1(x);
        RecordingTimelineCursor().X2(x);
        RecordingTimelineCursorText().Text(
            m_replay_loaded
                ? format_duration(m_replay_position_ns) +
                    L" / " +
                    format_duration(duration)
                : hstring{ L"00:00:00" });

        if (m_replay_loaded)
        {
            RecordingWorkspaceModeText().Text(
                m_replay_playing
                    ? L"● REPLAY"
                    : (m_replay_position_ns >= duration
                        ? L"COMPLETE"
                        : L"PAUSED"));
        }
    }

    void MainWindow::RefreshRecordingEvents()
    {
        if (!RecordingEventListView())
        {
            return;
        }
        RecordingEventListView().Items().Clear();
        for (auto const& event : m_recording_events)
        {
            StackPanel panel;
            panel.Spacing(2.0);
            TextBlock time;
            time.Text(
                format_duration(event.timestamp_ns) +
                L" · event " +
                to_hstring(event.event_type));
            time.FontFamily(FontFamily{ L"Consolas" });
            time.FontSize(11.0);
            time.Foreground(
                Application::Current().Resources().Lookup(
                    box_value(hstring{ L"MutedTextBrush" })).as<Brush>());
            panel.Children().Append(time);
            TextBlock message;
            message.Text(to_hstring(event.message));
            message.TextWrapping(TextWrapping::Wrap);
            panel.Children().Append(message);
            RecordingEventListView().Items().Append(panel);
        }
        RecordingEventCountText().Text(
            to_hstring(m_recording_events.size()) +
            (m_recording_events.size() == 1u
                ? L" event"
                : L" events"));
    }

    void MainWindow::RecordingAddMarker_Click(
        IInspectable const&,
        RoutedEventArgs const&)
    {
        if (!m_replay_loaded)
        {
            AddTimelineEvent(L"Open a recording before adding a marker");
            return;
        }
        hstring const note = RecordingMarkerTextBox().Text();
        std::wstring trimmed{ note.c_str() };
        auto const first = trimmed.find_first_not_of(L" \t\r\n");
        if (first == std::wstring::npos)
        {
            AddTimelineEvent(L"Enter a marker note");
            return;
        }
        trimmed.erase(0u, first);
        auto const last = trimmed.find_last_not_of(L" \t\r\n");
        trimmed.erase(last + 1u);

        auto const previous_markers = m_recording_markers;
        m_recording_markers.push_back(
            RecordingMarker{ m_replay_position_ns, hstring{ trimmed } });
        std::sort(
            m_recording_markers.begin(),
            m_recording_markers.end(),
            [](auto const& left, auto const& right)
            {
                return left.timestamp_ns < right.timestamp_ns;
            });
        if (!SaveRecordingMarkers())
        {
            m_recording_markers = previous_markers;
            AddTimelineEvent(L"Unable to save recording annotation");
            return;
        }
        RecordingMarkerTextBox().Text(L"");
        RefreshRecordingMarkers();
        RefreshRecordingCaptures();
        AddTimelineEvent(
            L"Replay marker added at " +
            format_duration(m_replay_position_ns));
    }

    bool MainWindow::SaveRecordingMarkers()
    {
        if (!m_replay_loaded)
        {
            return false;
        }
        std::ofstream stream{
            m_session_replay.Info().session_path / L"annotations.csv",
            std::ios::binary | std::ios::trunc
        };
        if (!stream)
        {
            return false;
        }
        stream << "timestamp_ns,note\r\n";
        for (auto const& marker : m_recording_markers)
        {
            std::string note = to_string(marker.note);
            std::string escaped;
            escaped.reserve(note.size() + 2u);
            escaped.push_back('"');
            for (char character : note)
            {
                if (character == '"')
                {
                    escaped.push_back('"');
                }
                escaped.push_back(character);
            }
            escaped.push_back('"');
            stream << marker.timestamp_ns << ',' << escaped << "\r\n";
        }
        return stream.good();
    }

    void MainWindow::LoadRecordingMarkers()
    {
        m_recording_markers.clear();
        if (!m_replay_loaded)
        {
            return;
        }
        std::ifstream stream{
            m_session_replay.Info().session_path / L"annotations.csv",
            std::ios::binary
        };
        if (!stream)
        {
            return;
        }

        std::string line;
        std::getline(stream, line);
        while (std::getline(stream, line))
        {
            if (!line.empty() && line.back() == '\r')
            {
                line.pop_back();
            }
            size_t const separator = line.find(',');
            if (separator == std::string::npos)
            {
                continue;
            }
            uint64_t timestamp = 0u;
            try
            {
                timestamp = std::stoull(line.substr(0u, separator));
            }
            catch (...)
            {
                continue;
            }

            std::string raw = line.substr(separator + 1u);
            std::string note;
            if (raw.size() >= 2u && raw.front() == '"' && raw.back() == '"')
            {
                for (size_t index = 1u; index + 1u < raw.size(); ++index)
                {
                    if (raw[index] == '"' &&
                        index + 1u < raw.size() - 1u &&
                        raw[index + 1u] == '"')
                    {
                        note.push_back('"');
                        ++index;
                    }
                    else
                    {
                        note.push_back(raw[index]);
                    }
                }
            }
            else
            {
                note = std::move(raw);
            }
            m_recording_markers.push_back(
                RecordingMarker{ timestamp, to_hstring(note) });
        }
        std::sort(
            m_recording_markers.begin(),
            m_recording_markers.end(),
            [](auto const& left, auto const& right)
            {
                return left.timestamp_ns < right.timestamp_ns;
            });
    }

    void MainWindow::RefreshRecordingMarkers()
    {
        if (!RecordingMarkerListView())
        {
            return;
        }
        RecordingMarkerListView().Items().Clear();
        for (auto const& marker : m_recording_markers)
        {
            StackPanel panel;
            panel.Spacing(2.0);
            TextBlock time;
            time.Text(format_duration(marker.timestamp_ns));
            time.FontFamily(FontFamily{ L"Consolas" });
            time.Foreground(
                Application::Current().Resources().Lookup(
                    box_value(hstring{ L"MeasurementAccentBrush" })).as<Brush>());
            panel.Children().Append(time);
            TextBlock note;
            note.Text(marker.note);
            note.TextWrapping(TextWrapping::Wrap);
            panel.Children().Append(note);

            ListViewItem row;
            row.Content(panel);
            row.Tag(box_value(marker.timestamp_ns));
            row.HorizontalContentAlignment(HorizontalAlignment::Stretch);
            RecordingMarkerListView().Items().Append(row);
        }
    }

    void MainWindow::RecordingMarkerList_ItemClick(
        IInspectable const&,
        ItemClickEventArgs const& args)
    {
        if (!m_replay_loaded)
        {
            return;
        }
        auto const item = args.ClickedItem().try_as<ListViewItem>();
        if (!item || !item.Tag())
        {
            return;
        }
        m_replay_playing = false;
        SeekReplay(unbox_value<uint64_t>(item.Tag()));
        UpdateReplayUi();
    }

    void MainWindow::RecordingCapturesRefresh_Click(
        IInspectable const&,
        RoutedEventArgs const&)
    {
        RefreshRecordingCaptures();
    }

    void MainWindow::RefreshRecordingCaptures()
    {
        if (!RecordingCaptureListView())
        {
            return;
        }
        RecordingCaptureListView().Items().Clear();
        std::filesystem::path const folder =
            m_replay_loaded
                ? m_session_replay.Info().session_path
                : m_recording_selected_path;
        std::error_code error;
        if (folder.empty() || !std::filesystem::is_directory(folder, error))
        {
            RecordingCaptureCountText().Text(L"No recording selected");
            return;
        }

        std::vector<std::filesystem::directory_entry> files;
        std::filesystem::directory_iterator iterator{ folder, error };
        std::filesystem::directory_iterator const end;
        while (!error && iterator != end)
        {
            auto const entry = *iterator;
            iterator.increment(error);
            if (entry.is_regular_file())
            {
                files.push_back(entry);
            }
        }
        std::sort(
            files.begin(),
            files.end(),
            [](auto const& left, auto const& right)
            {
                return left.path().filename().wstring() <
                    right.path().filename().wstring();
            });

        for (auto const& entry : files)
        {
            std::wstring const name = entry.path().filename().wstring();
            std::wstring extension = entry.path().extension().wstring();
            std::transform(
                extension.begin(),
                extension.end(),
                extension.begin(),
                [](wchar_t value)
                {
                    return static_cast<wchar_t>(std::towlower(value));
                });
            std::wstring kind{ L"Session file" };
            if (name == L"session.json")
            {
                kind = L"Manifest";
            }
            else if (name.starts_with(L"data-"))
            {
                kind = L"Measurement chunk";
            }
            else if (name == L"annotations.csv")
            {
                kind = L"Operator annotations";
            }
            else if (extension == L".iq" || extension == L".bin")
            {
                kind = L"IQ capture";
            }
            else if (extension == L".png" || extension == L".jpg")
            {
                kind = L"Screenshot";
            }
            else if (extension == L".csv")
            {
                kind = L"CSV export";
            }

            uintmax_t const size = entry.file_size(error);
            error.clear();
            StackPanel panel;
            panel.Spacing(2.0);
            TextBlock title;
            title.Text(hstring{ name });
            panel.Children().Append(title);
            TextBlock detail;
            wchar_t detail_buffer[128]{};
            swprintf_s(
                detail_buffer,
                L"%ls · %.1f kB",
                kind.c_str(),
                static_cast<double>(size) / 1024.0);
            detail.Text(detail_buffer);
            detail.FontSize(11.0);
            detail.Foreground(
                Application::Current().Resources().Lookup(
                    box_value(hstring{ L"MutedTextBrush" })).as<Brush>());
            panel.Children().Append(detail);
            RecordingCaptureListView().Items().Append(panel);
        }
        RecordingCaptureCountText().Text(
            to_hstring(files.size()) +
            (files.size() == 1u ? L" file" : L" files") +
            L" in " +
            hstring{ folder.filename().wstring() });
    }

    void MainWindow::RecordingCompare_Click(
        IInspectable const&,
        RoutedEventArgs const&)
    {
        if (!m_replay_loaded)
        {
            RecordingCompareDetailsText().Text(
                L"Open the baseline recording before comparing sessions.");
            return;
        }
        auto const selected =
            RecordingCompareSessionComboBox().SelectedItem()
                .try_as<ComboBoxItem>();
        if (!selected || !selected.Tag())
        {
            RecordingCompareDetailsText().Text(
                L"Select another recording to compare.");
            return;
        }

        dab::measurement::MeasurementSessionReplayInfo other;
        std::string error;
        if (!dab::measurement::MeasurementSessionReplay::ReadSessionInfo(
                std::filesystem::path{
                    unbox_value<hstring>(selected.Tag()).c_str()
                },
                other,
                error))
        {
            RecordingCompareDetailsText().Text(
                L"Unable to read comparison manifest · " +
                to_hstring(error));
            return;
        }

        auto const& active = m_session_replay.Info();
        RecordingCompareTitleText().Text(
            hstring{ active.session_path.filename().wstring() } +
            L" vs " +
            hstring{ other.session_path.filename().wstring() });
        wchar_t details[320]{};
        swprintf_s(
            details,
            L"%ls · %.3f MHz · %ls  |  %ls · %.3f MHz · %ls",
            to_hstring(active.channel.empty() ? "—" : active.channel).c_str(),
            static_cast<double>(active.frequency_hz) / 1000000.0,
            to_hstring(active.service.empty() ? "No service" : active.service).c_str(),
            to_hstring(other.channel.empty() ? "—" : other.channel).c_str(),
            static_cast<double>(other.frequency_hz) / 1000000.0,
            to_hstring(other.service.empty() ? "No service" : other.service).c_str());
        RecordingCompareDetailsText().Text(details);

        auto const signed_text =
            [](int64_t value, wchar_t const* suffix)
            {
                wchar_t buffer[80]{};
                swprintf_s(
                    buffer,
                    L"%+lld%ls",
                    static_cast<long long>(value),
                    suffix);
                return hstring{ buffer };
            };
        double const duration_delta =
            static_cast<double>(
                static_cast<int64_t>(other.duration_ns) -
                static_cast<int64_t>(active.duration_ns)) /
            1000000000.0;
        wchar_t duration_text[80]{};
        swprintf_s(duration_text, L"%+.1f s", duration_delta);
        RecordingCompareDurationText().Text(duration_text);
        RecordingCompareRecordsText().Text(
            signed_text(
                static_cast<int64_t>(other.record_count) -
                    static_cast<int64_t>(active.record_count),
                L""));
        double const size_delta =
            static_cast<double>(
                static_cast<int64_t>(other.bytes_written) -
                static_cast<int64_t>(active.bytes_written)) /
            (1024.0 * 1024.0);
        wchar_t size_text[80]{};
        swprintf_s(size_text, L"%+.1f MB", size_delta);
        RecordingCompareSizeText().Text(size_text);
        RecordingCompareDroppedText().Text(
            signed_text(
                static_cast<int64_t>(other.dropped_records) -
                    static_cast<int64_t>(active.dropped_records),
                L""));
    }

    winrt::fire_and_forget MainWindow::RecordingExportTimeline_Click(
        IInspectable const&,
        RoutedEventArgs const&)
    {
        auto lifetime = get_strong();
        if (!m_replay_loaded || m_recording_timeline.empty())
        {
            RecordingExportStatusBar().Severity(InfoBarSeverity::Warning);
            RecordingExportStatusBar().Title(L"Nothing to export");
            RecordingExportStatusBar().Message(
                L"Open a recording with measurement snapshots first.");
            RecordingExportStatusBar().IsOpen(true);
            co_return;
        }

        try
        {
            using namespace Windows::Storage;
            std::wstring csv{
                L"record_type,timestamp_ns,elapsed_s,power,power_unit,"
                L"mer_db,cn_db,cfo_hz,msc_ber,clipping_percent,locked,"
                L"dropped_blocks,note\r\n"
            };
            for (auto const& point : m_recording_timeline)
            {
                csv += L"measurement,";
                csv += std::to_wstring(point.timestamp_ns);
                csv += L",";
                csv += csv_float(
                    true,
                    static_cast<float>(
                        static_cast<double>(point.timestamp_ns) /
                        1000000000.0),
                    6);
                csv += L",";
                csv += csv_float(point.power_valid, point.power, 3);
                csv += L",";
                csv += point.power_valid
                    ? (point.power_calibrated ? L"dBm" : L"dBFS")
                    : L"";
                csv += L",";
                csv += csv_float(point.mer_valid, point.mer_db, 3);
                csv += L",";
                csv += csv_float(point.cn_valid, point.cn_db, 3);
                csv += L",";
                csv += csv_float(point.cfo_valid, point.cfo_hz, 3);
                csv += L",";
                csv += csv_float(point.msc_ber_valid, point.msc_ber, 8);
                csv += L",";
                csv += csv_float(
                    point.clipping_valid,
                    point.clipping_percent,
                    5);
                csv += L",";
                csv += point.locked ? L"1" : L"0";
                csv += L",";
                csv += std::to_wstring(point.dropped_blocks);
                csv += L",\r\n";
            }
            for (auto const& marker : m_recording_markers)
            {
                csv += L"marker,";
                csv += std::to_wstring(marker.timestamp_ns);
                csv += L",";
                csv += csv_float(
                    true,
                    static_cast<float>(
                        static_cast<double>(marker.timestamp_ns) /
                        1000000000.0),
                    6);
                csv += L",,,,,,,,,,,";
                csv += csv_field(marker.note.c_str());
                csv += L"\r\n";
            }

            auto const file = co_await DownloadsFolder::CreateFileAsync(
                L"DAB_Replay_Timeline_" +
                    filename_timestamp() +
                    L".csv",
                CreationCollisionOption::GenerateUniqueName);
            co_await FileIO::WriteTextAsync(file, hstring{ csv });
            RecordingExportStatusBar().Severity(InfoBarSeverity::Success);
            RecordingExportStatusBar().Title(L"Timeline exported");
            RecordingExportStatusBar().Message(file.Path());
            RecordingExportStatusBar().IsOpen(true);
            AddTimelineEvent(L"Replay timeline exported to " + file.Path());
        }
        catch (hresult_error const& error)
        {
            RecordingExportStatusBar().Severity(InfoBarSeverity::Error);
            RecordingExportStatusBar().Title(L"Export failed");
            RecordingExportStatusBar().Message(error.message());
            RecordingExportStatusBar().IsOpen(true);
        }
    }

    void MainWindow::RecordingCopySummary_Click(
        IInspectable const&,
        RoutedEventArgs const&)
    {
        if (!m_replay_loaded)
        {
            RecordingExportStatusBar().Severity(InfoBarSeverity::Warning);
            RecordingExportStatusBar().Title(L"No active recording");
            RecordingExportStatusBar().Message(
                L"Open a recording before copying its summary.");
            RecordingExportStatusBar().IsOpen(true);
            return;
        }
        try
        {
            auto const& info = m_session_replay.Info();
            std::wstring summary{
                L"DAB Measurement Recording\r\nSession: "
            };
            summary += info.session_path.filename().wstring();
            summary += L"\r\nState: ";
            summary += to_hstring(info.state).c_str();
            summary += L"\r\nContext: ";
            summary += to_hstring(info.context).c_str();
            summary += L"\r\nDevice: ";
            summary += to_hstring(info.device).c_str();
            summary += L"\r\nChannel / frequency: ";
            summary += to_hstring(info.channel).c_str();
            summary += L" / ";
            summary += std::to_wstring(info.frequency_hz);
            summary += L" Hz\r\nEnsemble / service: ";
            summary += to_hstring(info.ensemble).c_str();
            summary += L" / ";
            summary += to_hstring(info.service).c_str();
            summary += L"\r\nDuration: ";
            summary += format_duration(info.duration_ns).c_str();
            summary += L"\r\nRecords: ";
            summary += std::to_wstring(info.record_count);
            summary += L" (";
            summary += std::to_wstring(info.snapshot_records);
            summary += L" snapshots, ";
            summary += std::to_wstring(info.measurement_records);
            summary += L" measurement frames, ";
            summary += std::to_wstring(info.catalog_records);
            summary += L" catalogues, ";
            summary += std::to_wstring(info.event_records);
            summary += L" events)\r\nChunks: ";
            summary += std::to_wstring(info.chunk_count);
            summary += L"\r\nBytes: ";
            summary += std::to_wstring(info.bytes_written);
            summary += L"\r\nRecorder drops: ";
            summary += std::to_wstring(info.dropped_records);
            summary += L"\r\nIntegrity: CRC validated\r\n";

            using namespace Windows::ApplicationModel::DataTransfer;
            DataPackage package;
            package.SetText(hstring{ summary });
            Clipboard::SetContent(package);
            Clipboard::Flush();
            RecordingExportStatusBar().Severity(InfoBarSeverity::Success);
            RecordingExportStatusBar().Title(L"Summary copied");
            RecordingExportStatusBar().Message(
                L"The validated session summary is on the clipboard.");
            RecordingExportStatusBar().IsOpen(true);
        }
        catch (hresult_error const& error)
        {
            RecordingExportStatusBar().Severity(InfoBarSeverity::Error);
            RecordingExportStatusBar().Title(L"Copy failed");
            RecordingExportStatusBar().Message(error.message());
            RecordingExportStatusBar().IsOpen(true);
        }
    }

    void MainWindow::UpdateRecordingWorkspace()
    {
        if (!RecordingWorkspaceContextText())
        {
            return;
        }
        if (m_replay_importing)
        {
            RecordingWorkspaceContextText().Text(
                L"Validating manifest, chunks, schemas and record CRCs");
            RecordingWorkspaceModeText().Text(L"IMPORTING");
            RecordingWorkspaceIntegrityText().Text(L"VALIDATING CRC");
            return;
        }
        if (!m_replay_loaded)
        {
            RecordingWorkspaceContextText().Text(L"No recording loaded");
            RecordingWorkspaceModeText().Text(L"IDLE");
            RecordingWorkspaceIntegrityText().Text(L"NO SESSION");
            RenderRecordingTimeline();
            RefreshRecordingEvents();
            RefreshRecordingMarkers();
            RefreshRecordingCaptures();
            UpdateRecordingSelectedSession();
            return;
        }

        auto const& info = m_session_replay.Info();
        wchar_t frequency[48]{};
        swprintf_s(
            frequency,
            L"%.3f MHz",
            static_cast<double>(info.frequency_hz) / 1000000.0);
        RecordingWorkspaceContextText().Text(
            hstring{ info.session_path.filename().wstring() } +
            L" · " +
            to_hstring(info.channel.empty() ? "—" : info.channel) +
            L" · " +
            frequency +
            L" · " +
            to_hstring(info.ensemble.empty() ? "No ensemble" : info.ensemble) +
            L" · " +
            to_hstring(info.service.empty() ? "No service" : info.service));
        RecordingWorkspaceIntegrityText().Text(
            info.dropped_records == 0u
                ? L"CRC VALIDATED"
                : L"VALIDATED · " +
                    to_hstring(info.dropped_records) +
                    L" DROPPED");
        UpdateRecordingTimelineCursor();
        UpdateRecordingSelectedSession();
        RenderRecordingTimeline();
        RefreshRecordingEvents();
        RefreshRecordingMarkers();
        RefreshRecordingCaptures();
    }

    void MainWindow::MeasurementRfPage_Click(
        IInspectable const& sender,
        RoutedEventArgs const&)
    {
        auto const item = sender.try_as<MenuFlyoutItem>();
        if (!item || !item.Tag())
        {
            return;
        }

        std::wstring const value{ unbox_value<hstring>(item.Tag()).c_str() };
        wchar_t* end = nullptr;
        long const index = std::wcstol(value.c_str(), &end, 10);
        if (end == value.c_str() || index < 0 || index >= 7)
        {
            return;
        }

        ShowWorkspace(WorkspaceKind::rf_measurements);
        RfTabView().SelectedIndex(static_cast<int32_t>(index));
    }

    void MainWindow::MeasurementDabPage_Click(
        IInspectable const& sender,
        RoutedEventArgs const&)
    {
        auto const item = sender.try_as<MenuFlyoutItem>();
        if (!item || !item.Tag())
        {
            return;
        }

        std::wstring const tag{ unbox_value<hstring>(item.Tag()).c_str() };
        ShowWorkspace(WorkspaceKind::dab_measurements);
        if (tag == L"overview")
        {
            DabTabView().SelectedIndex(0);
            return;
        }
        if (tag == L"events")
        {
            DabTabView().SelectedIndex(4);
            return;
        }

        size_t const separator = tag.find(L':');
        if (separator == std::wstring::npos)
        {
            return;
        }
        wchar_t* end = nullptr;
        long const page = std::wcstol(tag.c_str() + separator + 1u, &end, 10);
        if (end == tag.c_str() + separator + 1u || page < 0)
        {
            return;
        }

        std::wstring_view const section{ tag.data(), separator };
        if (section == L"fic" && page < 5)
        {
            DabTabView().SelectedIndex(1);
            FicTabView().SelectedIndex(static_cast<int32_t>(page));
        }
        else if (section == L"msc" && page < 6)
        {
            DabTabView().SelectedIndex(2);
            MscTabView().SelectedIndex(static_cast<int32_t>(page));
        }
        else if (section == L"dabplus" && page < 6)
        {
            DabTabView().SelectedIndex(3);
            DabPlusMeasurementTabView().SelectedIndex(static_cast<int32_t>(page));
        }
    }

    void MainWindow::MeasurementCaptureIq_Click(
        IInspectable const& sender,
        RoutedEventArgs const& args)
    {
        ShowWorkspace(WorkspaceKind::rf_measurements);
        RfTabView().SelectedIndex(3);
        RfNullCaptureIq_Click(sender, args);
    }

    void MainWindow::MeasurementAddAnnotation_Click(
        IInspectable const& sender,
        RoutedEventArgs const& args)
    {
        MarkerButton_Click(sender, args);
    }

    void MainWindow::MeasurementAlarmThresholds_Click(
        IInspectable const&,
        RoutedEventArgs const&)
    {
        ShowWorkspace(WorkspaceKind::rf_measurements);
        RfTabView().SelectedIndex(6);
        RfHistoryAlarmMeasurementComboBox().Focus(FocusState::Programmatic);
    }

    void MainWindow::ViewReceiverPlot_Click(
        IInspectable const& sender,
        RoutedEventArgs const&)
    {
        auto const item = sender.try_as<MenuFlyoutItem>();
        if (!item || !item.Tag())
        {
            return;
        }

        std::wstring const tag{ unbox_value<hstring>(item.Tag()).c_str() };
        if (tag == L"spectrum")
        {
            SetCirPanelMaximized(false);
            ShowWorkspace(WorkspaceKind::receiver);
            return;
        }
        if (tag == L"timeline")
        {
            SetCirPanelMaximized(false);
            ShowWorkspace(WorkspaceKind::receiver);
            TimelineList().Focus(FocusState::Programmatic);
            return;
        }
        constexpr std::wstring_view prefix{ L"analysis:" };
        if (!std::wstring_view{ tag }.starts_with(prefix))
        {
            return;
        }

        wchar_t* end = nullptr;
        long const index = std::wcstol(tag.c_str() + prefix.size(), &end, 10);
        if (end == tag.c_str() + prefix.size() || index < 0 || index >= 3)
        {
            return;
        }
        ShowWorkspace(WorkspaceKind::receiver);
        ReceiverAnalysisTabView().SelectedIndex(static_cast<int32_t>(index));
    }

    void MainWindow::ViewAnalysisPanelMaximized_Click(
        IInspectable const&,
        RoutedEventArgs const&)
    {
        bool const maximize = ViewAnalysisPanelMaximizedMenuItem().IsChecked();
        ShowWorkspace(WorkspaceKind::receiver);
        SetCirPanelMaximized(maximize);
        UpdateViewMenuState();
    }

    void MainWindow::ViewToolbar_Click(
        IInspectable const&,
        RoutedEventArgs const&)
    {
        m_toolbar_visible = ViewToolbarMenuItem().IsChecked();
        ReceiverToolbarPanel().Visibility(
            m_toolbar_visible ? Visibility::Visible : Visibility::Collapsed);
        UpdateViewMenuState();
    }

    void MainWindow::ViewStatusBar_Click(
        IInspectable const&,
        RoutedEventArgs const&)
    {
        m_status_bar_visible = ViewStatusBarMenuItem().IsChecked();
        StatusBarPanel().Visibility(
            m_status_bar_visible ? Visibility::Visible : Visibility::Collapsed);
        StatusBarRow().Height(GridLengthHelper::FromPixels(
            m_status_bar_visible ? 34.0 : 0.0));
        UpdateViewMenuState();
    }

    void MainWindow::ViewFullScreen_Click(
        IInspectable const&,
        RoutedEventArgs const&)
    {
        bool const full_screen = ViewFullScreenMenuItem().IsChecked();
        try
        {
            auto const app_window = AppWindow();
            app_window.SetPresenter(full_screen
                ? Microsoft::UI::Windowing::AppWindowPresenterKind::FullScreen
                : Microsoft::UI::Windowing::AppWindowPresenterKind::Default);
            m_full_screen = full_screen;
        }
        catch (hresult_error const& error)
        {
            m_full_screen = false;
            AddTimelineEvent(L"Unable to change full-screen mode: " + error.message());
        }
        UpdateViewMenuState();
    }

    void MainWindow::ViewResetLayout_Click(
        IInspectable const&,
        RoutedEventArgs const&)
    {
        SetCirPanelMaximized(false);
        m_toolbar_visible = true;
        m_status_bar_visible = true;
        ReceiverToolbarPanel().Visibility(Visibility::Visible);
        StatusBarPanel().Visibility(Visibility::Visible);
        StatusBarRow().Height(GridLengthHelper::FromPixels(34.0));
        if (m_full_screen)
        {
            try
            {
                AppWindow().SetPresenter(
                    Microsoft::UI::Windowing::AppWindowPresenterKind::Default);
            }
            catch (hresult_error const& error)
            {
                AddTimelineEvent(L"Unable to restore window presenter: " + error.message());
            }
            m_full_screen = false;
        }
        UpdateViewMenuState();
    }

    winrt::fire_and_forget MainWindow::ToolsOpenCaptureFolder_Click(
        IInspectable const&,
        RoutedEventArgs const&)
    {
        auto lifetime = get_strong();
        wchar_t local_app_data[MAX_PATH]{};
        DWORD const length = GetEnvironmentVariableW(
            L"LOCALAPPDATA",
            local_app_data,
            static_cast<DWORD>(std::size(local_app_data)));
        if (length == 0u || length >= std::size(local_app_data))
        {
            AddTimelineEvent(L"Unable to locate the triggered-IQ capture folder");
            co_return;
        }

        std::wstring folder{ local_app_data };
        folder += L"\\DabMeasurementWinUI";
        if (!CreateDirectoryW(folder.c_str(), nullptr) &&
            GetLastError() != ERROR_ALREADY_EXISTS)
        {
            AddTimelineEvent(L"Unable to create the triggered-IQ capture folder");
            co_return;
        }

        try
        {
            auto const capture_folder =
                co_await Windows::Storage::StorageFolder::GetFolderFromPathAsync(
                    folder);
            bool const opened =
                co_await Windows::System::Launcher::LaunchFolderAsync(
                    capture_folder);
            AddTimelineEvent(opened
                ? hstring{ L"Opened triggered-IQ capture folder" }
                : hstring{ L"Windows declined to open the triggered-IQ capture folder" });
        }
        catch (hresult_error const& error)
        {
            AddTimelineEvent(
                L"Unable to open the triggered-IQ capture folder: " +
                error.message());
        }
    }

    void MainWindow::ToolsRfCalibration_Click(
        IInspectable const&,
        RoutedEventArgs const&)
    {
        ShowWorkspace(WorkspaceKind::rf_measurements);
        RfTabView().SelectedIndex(7);
        UpdateCalibrationUi(m_have_latest_rf_snapshot
            ? &m_latest_rf_snapshot
            : nullptr);
    }

    winrt::fire_and_forget MainWindow::ToolsRecordingBrowser_Click(
        IInspectable const&,
        RoutedEventArgs const&)
    {
        auto lifetime = get_strong();
        auto const folder_path = measurement_recordings_root();
        std::error_code error;
        std::filesystem::create_directories(folder_path, error);
        if (error)
        {
            AddTimelineEvent(
                L"Unable to create the recordings folder");
            co_return;
        }
        try
        {
            auto const folder =
                co_await Windows::Storage::StorageFolder::GetFolderFromPathAsync(
                    folder_path.wstring());
            bool const opened =
                co_await Windows::System::Launcher::LaunchFolderAsync(folder);
            AddTimelineEvent(opened
                ? hstring{ L"Opened measurement recordings folder" }
                : hstring{ L"Windows declined to open the recordings folder" });
        }
        catch (hresult_error const& exception)
        {
            AddTimelineEvent(
                L"Unable to open the recordings folder: " +
                exception.message());
        }
    }

    void MainWindow::LoadCalibrationProfile()
    {
        std::wstring message;
        dab_rf_calibration_profile_init(&m_calibration_profile);
        m_calibration_profile_loaded =
            dab::measurement::CalibrationProfileStore::Load(
                m_calibration_profile,
                message);
        if (m_calibration_profile_loaded)
        {
            m_receiver.SetCalibrationProfile(m_calibration_profile);
        }
        else
        {
            m_receiver.ClearCalibrationProfile();
        }
        CalibrationProfilePathText().Text(hstring{
            dab::measurement::CalibrationProfileStore::ProfilePath().wstring()
        });
        CalibrationProfileNameTextBox().Text(
            m_calibration_profile.name[0] != '\0'
                ? to_hstring(m_calibration_profile.name)
                : hstring{ L"Band III local calibration" });
        RefreshCalibrationPointList();
        UpdateCalibrationUi();
        if (!message.empty())
        {
            AddTimelineEvent(hstring{ message });
        }
    }

    bool MainWindow::SaveCalibrationProfile()
    {
        std::wstring message;
        if (!dab::measurement::CalibrationProfileStore::Save(
                m_calibration_profile,
                message))
        {
            CalibrationMessageBar().Severity(InfoBarSeverity::Error);
            CalibrationMessageBar().Title(hstring{ message });
            CalibrationMessageBar().IsOpen(true);
            AddTimelineEvent(hstring{ message });
            return false;
        }
        m_calibration_profile_loaded = true;
        m_receiver.SetCalibrationProfile(m_calibration_profile);
        RefreshCalibrationPointList();
        AddTimelineEvent(hstring{ message });
        return true;
    }

    bool MainWindow::CalibrationWindowStatistics(
        float& mean_dbfs,
        float& standard_deviation_db,
        size_t& sample_count) const
    {
        size_t const wanted = (std::min)(m_rf_history_count, size_t{ 8u });
        double sum = 0.0;
        double sum_squared = 0.0;
        sample_count = 0u;
        for (size_t offset = 0u; offset < wanted; ++offset)
        {
            size_t const index =
                (m_rf_history_write + rf_history_capacity - 1u - offset) %
                rf_history_capacity;
            float const value = m_rf_power_history[index];
            if (!std::isfinite(value))
            {
                continue;
            }
            sum += value;
            sum_squared += static_cast<double>(value) * value;
            ++sample_count;
        }
        if (sample_count == 0u)
        {
            mean_dbfs = 0.0f;
            standard_deviation_db = 0.0f;
            return false;
        }
        double const mean = sum / static_cast<double>(sample_count);
        double const variance = (std::max)(
            0.0,
            sum_squared / static_cast<double>(sample_count) - mean * mean);
        mean_dbfs = static_cast<float>(mean);
        standard_deviation_db = static_cast<float>(std::sqrt(variance));
        return true;
    }

    void MainWindow::RefreshCalibrationPointList()
    {
        if (!CalibrationPointsList())
        {
            return;
        }
        CalibrationPointsList().Items().Clear();
        for (uint32_t index = 0u;
             index < m_calibration_profile.point_count;
             ++index)
        {
            auto const& point = m_calibration_profile.points[index];
            Grid row;
            row.ColumnSpacing(8.0);
            std::array<double, 7u> const widths{
                120.0, 90.0, 110.0, 130.0, 130.0, 120.0, 120.0
            };
            for (double width : widths)
            {
                ColumnDefinition column;
                column.Width(GridLengthHelper::FromPixels(width));
                row.ColumnDefinitions().Append(column);
            }

            wchar_t values[7][48]{};
            swprintf_s(values[0], L"%.3f MHz",
                       static_cast<double>(point.frequency_hz) / 1000000.0);
            swprintf_s(values[1], L"State %u", point.lna_state);
            swprintf_s(values[2], L"%d dB", point.gain_reduction_db);
            swprintf_s(values[3], L"%.2f dBm",
                       static_cast<double>(point.reference_power_dbm));
            swprintf_s(values[4], L"%.2f dBFS",
                       static_cast<double>(point.measured_power_dbfs));
            swprintf_s(values[5], L"%+.2f dB",
                       static_cast<double>(point.correction_db));
            swprintf_s(values[6], L"\u00b1%.2f dB",
                       static_cast<double>(point.uncertainty_db));
            for (int32_t column = 0; column < 7; ++column)
            {
                TextBlock text;
                text.Text(values[column]);
                text.FontFamily(FontFamily{ L"Consolas" });
                Grid::SetColumn(text, column);
                row.Children().Append(text);
            }

            ListViewItem item;
            item.HorizontalContentAlignment(HorizontalAlignment::Stretch);
            item.Content(row);
            CalibrationPointsList().Items().Append(item);
        }
        CalibrationProfilePointCountText().Text(
            to_hstring(m_calibration_profile.point_count) +
            (m_calibration_profile.point_count == 1u
                ? hstring{ L" point" }
                : hstring{ L" points" }));
    }

    void MainWindow::UpdateCalibrationUi(
        dab_receive_snapshot_t const* snapshot)
    {
        if (!CalibrationProfileStatusText())
        {
            return;
        }
        if (snapshot != nullptr)
        {
            m_latest_rf_snapshot = *snapshot;
            m_have_latest_rf_snapshot = true;
        }
        auto const* current = m_have_latest_rf_snapshot
            ? &m_latest_rf_snapshot
            : nullptr;

        CalibrationProfilePathText().Text(hstring{
            dab::measurement::CalibrationProfileStore::ProfilePath().wstring()
        });
        if (current != nullptr)
        {
            CalibrationCurrentRawText().Text(current->adc_level_valid
                ? format_float(current->adc_level_dbfs, L" dBFS", 2)
                : hstring{ L"\u2014 dBFS" });
            CalibrationCurrentPowerText().Text(
                current->calibrated_power_valid
                    ? format_float(current->calibrated_power_dbm,
                                   L" dBm",
                                   2)
                    : hstring{ L"Not calibrated" });
            CalibrationCorrectionText().Text(
                current->calibrated_power_valid
                    ? format_signed_float(current->calibration_correction_db,
                                          L" dB",
                                          2)
                    : hstring{ L"\u2014 dB" });
            CalibrationUncertaintyText().Text(
                current->calibrated_power_valid
                    ? hstring{ L"\u00b1" } +
                        format_float(current->calibrated_power_uncertainty_db,
                                     L" dB",
                                     2)
                    : hstring{ L"\u2014 dB" });

            hstring serial = current->device_serial[0] != '\0'
                ? to_hstring(current->device_serial)
                : hstring{ L"not reported" };
            wchar_t device[160]{};
            swprintf_s(device,
                       L"%hs \u00b7 Serial %ls \u00b7 HW %u \u00b7 API %.2f",
                       current->device_driver,
                       serial.c_str(),
                       current->device_hardware_version,
                       static_cast<double>(current->device_api_version));
            CalibrationDeviceText().Text(device);

            wchar_t settings[220]{};
            swprintf_s(settings,
                       L"%.3f MHz \u00b7 %.3f MS/s \u00b7 BW %.3f MHz\r\n"
                       L"LNA state %u \u00b7 applied GR %d dB \u00b7 generation %u",
                       static_cast<double>(current->frequency_hz) / 1000000.0,
                       static_cast<double>(current->sample_rate_hz) / 1000000.0,
                       static_cast<double>(current->device_bandwidth_hz) / 1000000.0,
                       current->lna_state,
                       current->if_gain_reduction_db,
                       current->gain_change_generation);
            CalibrationSettingsText().Text(settings);
        }
        else
        {
            CalibrationCurrentRawText().Text(L"\u2014 dBFS");
            CalibrationCurrentPowerText().Text(L"Not calibrated");
            CalibrationCorrectionText().Text(L"\u2014 dB");
            CalibrationUncertaintyText().Text(L"\u2014 dB");
            CalibrationDeviceText().Text(L"Device identity not reported");
            CalibrationSettingsText().Text(L"Frequency and gain state unavailable");
        }

        hstring badge{ L"UNCALIBRATED" };
        hstring profile_status{ L"No active profile" };
        auto status_brush = Application::Current().Resources().Lookup(
            box_value(hstring{ L"WarningBrush" })).as<Brush>();
        if (m_calibration_profile_loaded && current == nullptr)
        {
            badge = L"PROFILE ACTIVE";
            profile_status = L"Profile active \u00b7 waiting for receiver telemetry";
        }
        else if (m_calibration_profile_loaded && current != nullptr &&
                 current->calibrated_power_valid)
        {
            wchar_t calibrated_badge[64]{};
            swprintf_s(calibrated_badge,
                       L"\u25cf CALIBRATED \u00b1%.1f dB",
                       static_cast<double>(
                           current->calibrated_power_uncertainty_db));
            badge = calibrated_badge;
            profile_status =
                to_hstring(m_calibration_profile.name) +
                L" \u00b7 matched current operating point";
            status_brush = Application::Current().Resources().Lookup(
                box_value(hstring{ L"MeasurementAccentBrush" })).as<Brush>();
        }
        else if (m_calibration_profile_loaded && current != nullptr)
        {
            badge = L"PROFILE MISMATCH";
            profile_status =
                to_hstring(m_calibration_profile.name) +
                L" \u00b7 " +
                to_hstring(dab_rf_calibration_status_text(
                    current->calibration_status));
        }
        RfCalibrationBadgeText().Text(badge);
        RfCalibrationBadgeText().Foreground(status_brush);
        StatusCalibrationText().Text(badge);
        StatusCalibrationText().Foreground(status_brush);
        CalibrationProfileStatusText().Text(profile_status);
        CalibrationProfileStatusText().Foreground(status_brush);
        RfConfigCalibrationText().Text(profile_status);
        RfConfigCalibrationText().Foreground(status_brush);

        float mean = 0.0f;
        float deviation = 0.0f;
        size_t count = 0u;
        bool const stable = CalibrationWindowStatistics(mean,
                                                        deviation,
                                                        count) &&
                            count >= 4u &&
                            deviation <= 0.25f;
        bool profile_compatible = true;
        if (m_calibration_profile_loaded && current != nullptr)
        {
            profile_compatible =
                std::strcmp(m_calibration_profile.driver,
                            current->device_driver) == 0 &&
                std::strcmp(m_calibration_profile.device_serial,
                            current->device_serial) == 0 &&
                m_calibration_profile.hardware_version ==
                    current->device_hardware_version &&
                m_calibration_profile.sample_rate_hz ==
                    current->sample_rate_hz &&
                m_calibration_profile.bandwidth_hz ==
                    current->device_bandwidth_hz &&
                m_calibration_profile.if_mode == current->device_if_mode &&
                m_calibration_profile.antenna == current->device_antenna;
        }
        bool const can_capture =
            current != nullptr &&
            std::strcmp(current->device_driver, "sdrplay") == 0 &&
            current->device_serial[0] != '\0' &&
            current->adc_level_valid != 0 &&
            current->applied_gain_valid != 0 &&
            current->agc_enabled == 0 &&
            current->adc_clipped_sample_count == 0u &&
            stable &&
            profile_compatible;
        CalibrationCaptureButton().IsEnabled(can_capture);

        wchar_t condition[256]{};
        if (current == nullptr)
        {
            swprintf_s(condition, L"Start the receiver with SDRplay and AGC disabled.");
        }
        else if (current->agc_enabled != 0)
        {
            swprintf_s(condition, L"AGC is active. Disable AGC and restart before calibration.");
        }
        else if (current->applied_gain_valid == 0 ||
                 current->device_serial[0] == '\0')
        {
            swprintf_s(condition, L"Waiting for applied SDRplay gain and device identity.");
        }
        else if (!profile_compatible)
        {
            swprintf_s(condition, L"The active profile belongs to different hardware or receiver settings. Clear it before creating a new profile.");
        }
        else if (current->adc_clipped_sample_count != 0u)
        {
            swprintf_s(condition, L"ADC clipping detected. Reduce the reference source level.");
        }
        else if (!stable)
        {
            swprintf_s(condition,
                       L"Collecting a stable window: %zu/8 samples \u00b7 \u03c3 %.3f dB (limit 0.250 dB).",
                       count,
                       static_cast<double>(deviation));
        }
        else
        {
            swprintf_s(condition,
                       L"Ready: mean %.3f dBFS \u00b7 \u03c3 %.3f dB \u00b7 manual gain confirmed.",
                       static_cast<double>(mean),
                       static_cast<double>(deviation));
        }
        CalibrationConditionText().Text(condition);
        CalibrationMessageBar().Severity(
            can_capture ? InfoBarSeverity::Success : InfoBarSeverity::Informational);
        CalibrationMessageBar().Title(
            can_capture
                ? hstring{ L"Operating point is stable and ready to capture" }
                : hstring{ condition });
        CalibrationMessageBar().IsOpen(true);
    }

    void MainWindow::CalibrationCapturePoint_Click(
        IInspectable const&,
        RoutedEventArgs const&)
    {
        if (!m_have_latest_rf_snapshot)
        {
            return;
        }
        float mean_dbfs = 0.0f;
        float deviation_db = 0.0f;
        size_t sample_count = 0u;
        if (!CalibrationWindowStatistics(mean_dbfs,
                                         deviation_db,
                                         sample_count) ||
            sample_count < 4u ||
            deviation_db > 0.25f)
        {
            UpdateCalibrationUi(&m_latest_rf_snapshot);
            return;
        }

        double const reference = CalibrationReferencePowerBox().Value();
        double const requested_uncertainty =
            CalibrationPointUncertaintyBox().Value();
        if (!std::isfinite(reference) ||
            !std::isfinite(requested_uncertainty) ||
            requested_uncertainty <= 0.0)
        {
            CalibrationMessageBar().Severity(InfoBarSeverity::Error);
            CalibrationMessageBar().Title(
                L"Enter a valid reference power and uncertainty");
            return;
        }

        auto const& snapshot = m_latest_rf_snapshot;
        if (!m_calibration_profile_loaded)
        {
            dab_rf_calibration_profile_init(&m_calibration_profile);
            std::snprintf(m_calibration_profile.id,
                          sizeof(m_calibration_profile.id),
                          "rsp-band3-%s-v1",
                          snapshot.device_serial);
            std::string const name =
                to_string(CalibrationProfileNameTextBox().Text());
            std::snprintf(m_calibration_profile.name,
                          sizeof(m_calibration_profile.name),
                          "%s",
                          name.empty() ? "Band III local calibration" : name.c_str());
            std::snprintf(m_calibration_profile.driver,
                          sizeof(m_calibration_profile.driver),
                          "%s",
                          snapshot.device_driver);
            std::snprintf(m_calibration_profile.device_serial,
                          sizeof(m_calibration_profile.device_serial),
                          "%s",
                          snapshot.device_serial);
            m_calibration_profile.hardware_version =
                snapshot.device_hardware_version;
            m_calibration_profile.sample_rate_hz = snapshot.sample_rate_hz;
            m_calibration_profile.bandwidth_hz =
                snapshot.device_bandwidth_hz;
            m_calibration_profile.if_mode = snapshot.device_if_mode;
            m_calibration_profile.antenna = snapshot.device_antenna;
            m_calibration_profile.default_uncertainty_db =
                static_cast<float>(requested_uncertainty);
        }

        dab_rf_calibration_point_t point{};
        point.frequency_hz = snapshot.frequency_hz;
        point.lna_state = snapshot.lna_state;
        point.gain_reduction_db = snapshot.if_gain_reduction_db;
        point.reference_power_dbm = static_cast<float>(reference);
        point.measured_power_dbfs = mean_dbfs;
        point.uncertainty_db = (std::max)(
            static_cast<float>(requested_uncertainty),
            deviation_db);
        if (dab_rf_calibration_profile_add_point(
                &m_calibration_profile,
                &point) != 0 ||
            !SaveCalibrationProfile())
        {
            return;
        }

        CalibrationMessageBar().Severity(InfoBarSeverity::Success);
        CalibrationMessageBar().Title(
            L"Calibration point saved; the next receiver snapshot will use it");
        CalibrationMessageBar().IsOpen(true);
        AddTimelineEvent(
            L"RF calibration point captured at " +
            format_float(static_cast<float>(snapshot.frequency_hz) / 1000000.0f,
                         L" MHz",
                         3));
    }

    void MainWindow::CalibrationClearProfile_Click(
        IInspectable const&,
        RoutedEventArgs const&)
    {
        std::error_code error;
        std::filesystem::remove(
            dab::measurement::CalibrationProfileStore::ProfilePath(),
            error);
        dab_rf_calibration_profile_init(&m_calibration_profile);
        m_calibration_profile_loaded = false;
        m_receiver.ClearCalibrationProfile();
        CalibrationProfileNameTextBox().Text(L"Band III local calibration");
        RefreshCalibrationPointList();
        UpdateCalibrationUi(m_have_latest_rf_snapshot
            ? &m_latest_rf_snapshot
            : nullptr);
        CalibrationMessageBar().Severity(InfoBarSeverity::Informational);
        CalibrationMessageBar().Title(L"RF calibration profile cleared");
        AddTimelineEvent(L"RF calibration profile cleared");
    }

    winrt::fire_and_forget MainWindow::CalibrationOpenFolder_Click(
        IInspectable const&,
        RoutedEventArgs const&)
    {
        auto lifetime = get_strong();
        auto const folder =
            dab::measurement::CalibrationProfileStore::ProfilePath().parent_path();
        std::error_code error;
        std::filesystem::create_directories(folder, error);
        if (error)
        {
            AddTimelineEvent(L"Unable to create the RF calibration profile folder");
            co_return;
        }
        try
        {
            auto const storage_folder =
                co_await Windows::Storage::StorageFolder::GetFolderFromPathAsync(
                    folder.wstring());
            bool const opened =
                co_await Windows::System::Launcher::LaunchFolderAsync(
                    storage_folder);
            AddTimelineEvent(opened
                ? hstring{ L"Opened RF calibration profile folder" }
                : hstring{ L"Windows declined to open the RF calibration profile folder" });
        }
        catch (hresult_error const& error_message)
        {
            AddTimelineEvent(
                L"Unable to open the RF calibration profile folder: " +
                error_message.message());
        }
    }

    void MainWindow::ToolsCopyDiagnosticSummary_Click(
        IInspectable const&,
        RoutedEventArgs const&)
    {
        try
        {
            std::wstring summary =
                L"DAB+ Measurement Workbench - session diagnostics\r\n";
            summary += L"Captured: ";
            summary += local_clock_text().c_str();
            summary += L"\r\nReceiver: ";
            summary += m_receiver.IsActive() ? L"active" : L"stopped";
            summary += L"\r\nDevice: ";
            summary += StatusDeviceText().Text().c_str();
            summary += L"\r\nChannel: ";
            summary += StatusChannelText().Text().c_str();
            summary += L"\r\nFrequency: ";
            summary += FrequencyTextBox().Text().c_str();

            double gain = GainNumberBox().Value();
            if (!std::isfinite(gain))
            {
                gain = 40.0;
            }
            wchar_t front_end[160]{};
            swprintf_s(front_end,
                       L"\r\nFront end: gain %ld dB, AGC %ls, PPM %+d, bias tee %ls",
                       std::lround(gain),
                       m_receiver_agc_enabled ? L"enabled" : L"disabled",
                       m_receiver_ppm_correction,
                       m_receiver_bias_tee_enabled ? L"enabled" : L"disabled");
            summary += front_end;

            summary += L"\r\nSynchronization: ";
            summary += StatusSyncText().Text().c_str();
            summary += L"\r\nMeasurement state: ";
            summary += DataModeText().Text().c_str();
            summary += m_metrics_stale ? L" (stale)" : L"";
            summary += L"\r\nReceiver detail: ";
            summary += DataSeamText().Text().c_str();
            summary += L"\r\nRF power: ";
            summary += RfPowerValueText().Text().c_str();
            summary += L"\r\nC/N: ";
            summary += RfCnValueText().Text().c_str();
            summary += L"\r\nMER: ";
            summary += RfMerValueText().Text().c_str();
            summary += L"\r\nCFO: ";
            summary += RfCfoValueText().Text().c_str();
            summary += L"\r\nOverruns: ";
            summary += StatusOverrunText().Text().c_str();
            summary += L"\r\nCPU: ";
            summary += StatusCpuText().Text().c_str();
            summary += L"\r\nDAB context: ";
            summary += DabContextText().Text().c_str();
            summary += L"\r\nDAB component: ";
            summary += DabComponentContextText().Text().c_str();
            summary += L"\r\n";

            using namespace Windows::ApplicationModel::DataTransfer;
            DataPackage package;
            package.SetText(hstring{ summary });
            Clipboard::SetContent(package);
            AddTimelineEvent(L"Session diagnostic summary copied to clipboard");
        }
        catch (hresult_error const& error)
        {
            AddTimelineEvent(L"Unable to copy diagnostic summary: " + error.message());
        }
    }

    winrt::fire_and_forget MainWindow::ShowHelpDialog(
        hstring const& title,
        hstring const& content)
    {
        auto lifetime = get_strong();
        try
        {
            TextBlock body;
            body.Text(content);
            body.TextWrapping(TextWrapping::Wrap);
            body.IsTextSelectionEnabled(true);
            body.LineHeight(21.0);

            ScrollViewer scroll;
            scroll.MaxWidth(760.0);
            scroll.MaxHeight(560.0);
            scroll.HorizontalScrollBarVisibility(ScrollBarVisibility::Disabled);
            scroll.VerticalScrollBarVisibility(ScrollBarVisibility::Auto);
            scroll.Content(body);

            ContentDialog dialog;
            dialog.XamlRoot(RootLayout().XamlRoot());
            dialog.Title(box_value(title));
            dialog.Content(scroll);
            dialog.CloseButtonText(L"Close");
            co_await dialog.ShowAsync();
        }
        catch (hresult_error const& error)
        {
            AddTimelineEvent(L"Unable to show help: " + error.message());
        }
    }

    void MainWindow::HelpQuickStart_Click(
        IInspectable const&,
        RoutedEventArgs const&)
    {
        ShowHelpDialog(
            L"Quick start",
            L"1. Select the SDR device and Band III channel in the receiver toolbar.\r\n\r\n"
            L"2. Set gain, AGC, PPM correction, and bias tee from Receiver before starting. "
            L"Hardware settings are intentionally locked while streaming.\r\n\r\n"
            L"3. Press F5 or choose Receiver > Start receiver. Wait for RF lock and a valid FIC catalogue.\r\n\r\n"
            L"4. Select an audio service in the Receiver workspace. Selecting a different service restarts only "
            L"the receiver session needed to decode that component.\r\n\r\n"
            L"5. Use RF Measurements for input level, spectrum, synchronization, null symbol, CIR, and history. "
            L"Use DAB Measurements for FIC, MSC, DAB+, audio continuity, and metadata.\r\n\r\n"
            L"6. Save snapshots and CSV exports from File. Triggered IQ is captured from the latest null-scope "
            L"window and stored under LocalAppData\\DabMeasurementWinUI.\r\n\r\n"
            L"Important: dBFS is relative to ADC full scale. Treat a dBm value as absolute received power only "
            L"when a valid device calibration profile is active.");
    }

    void MainWindow::HelpShortcuts_Click(
        IInspectable const&,
        RoutedEventArgs const&)
    {
        ShowHelpDialog(
            L"Keyboard and mouse shortcuts",
            L"F1                     Quick start\r\n"
            L"F5                     Start or stop the receiver\r\n"
            L"F11                    Enter or leave full screen\r\n"
            L"Ctrl+N                 New measurement session\r\n"
            L"Ctrl+W                 Close measurement session\r\n"
            L"Ctrl+S                 Save measurement snapshot\r\n"
            L"Ctrl+E                 Export current measurements as CSV\r\n"
            L"Ctrl+L                 Focus and select the frequency field\r\n"
            L"Ctrl+M                 Enlarge or restore the receiver analysis panel\r\n\r\n"
            L"Spectrum interaction\r\n"
            L"Mouse wheel            Zoom around the pointer frequency\r\n"
            L"Drag                   Pan the visible span\r\n"
            L"Double-click           Reset spectrum view\r\n"
            L"Click                  Place the active marker\r\n"
            L"Drag marker            Move an existing marker\r\n"
            L"Right-click            Open tune/reset actions\r\n\r\n"
            L"CIR interaction\r\n"
            L"Click a CIR peak       Place a path marker and show delay, relative power, and distance difference");
    }

    void MainWindow::HelpTerminology_Click(
        IInspectable const&,
        RoutedEventArgs const&)
    {
        ShowHelpDialog(
            L"RF and DAB measurement terminology",
            L"dBFS — Level relative to ADC full scale. 0 dBFS is the clipping limit; it is not received power in dBm.\r\n\r\n"
            L"dBm — Absolute power relative to 1 mW. It is trustworthy only with a calibration that includes the device, gain state, and signal path.\r\n\r\n"
            L"C/N — Carrier-to-noise ratio, estimated using the DAB null or another noise estimate.\r\n\r\n"
            L"MER — Modulation error ratio. It compares ideal and measured constellation vectors after synchronization/equalization; higher is better.\r\n\r\n"
            L"CFO — Carrier-frequency offset. Coarse CFO is the acquisition estimate, fine CFO is the tracking correction, and residual CFO is what remains.\r\n\r\n"
            L"CIR — Channel impulse response. Peaks after the main path indicate delayed propagation paths. Delay multiplied by propagation speed estimates path-distance difference.\r\n\r\n"
            L"FIC / FIB / FIG — The Fast Information Channel carries Fast Information Blocks containing Fast Information Groups that describe the ensemble.\r\n\r\n"
            L"MSC / CIF / subchannel — The Main Service Channel transports service data in Common Interleaved Frames allocated to protected subchannels.\r\n\r\n"
            L"Pre-Viterbi BER — A pseudo-BER or error estimate before convolutional decoding. Post-Viterbi errors are residual decoder failures.\r\n\r\n"
            L"RS corrections — Reed-Solomon bytes corrected in a DAB+ superframe. Rising corrections are an early warning even while audio remains valid.\r\n\r\n"
            L"AU CRC — Integrity check for a DAB+ audio access unit. Failure may cause concealment or muting.\r\n\r\n"
            L"Availability — Fraction of the session with valid audio output. Concealed and muted intervals are reported separately.\r\n\r\n"
            L"Stale — The displayed value is retained from an older valid snapshot because live receiver progress stopped or recovery is active.");
    }

    winrt::fire_and_forget MainWindow::HelpOpenReadme_Click(
        IInspectable const&,
        RoutedEventArgs const&)
    {
        auto lifetime = get_strong();
        wchar_t module_path[32768]{};
        DWORD const length = GetModuleFileNameW(
            nullptr,
            module_path,
            static_cast<DWORD>(std::size(module_path)));
        if (length == 0u || length >= std::size(module_path))
        {
            AddTimelineEvent(L"Unable to locate the dab-lib README");
            co_return;
        }

        std::wstring readme{ module_path, length };
        for (int level = 0; level < 5; ++level)
        {
            size_t const separator = readme.find_last_of(L"\\/");
            if (separator == std::wstring::npos)
            {
                AddTimelineEvent(L"Unable to resolve the dab-lib repository path");
                co_return;
            }
            readme.resize(separator);
        }
        readme += L"\\README.md";
        if (GetFileAttributesW(readme.c_str()) == INVALID_FILE_ATTRIBUTES)
        {
            AddTimelineEvent(L"dab-lib README was not found beside this development build");
            co_return;
        }

        try
        {
            auto const file =
                co_await Windows::Storage::StorageFile::GetFileFromPathAsync(
                    readme);
            bool const opened =
                co_await Windows::System::Launcher::LaunchFileAsync(file);
            AddTimelineEvent(opened
                ? hstring{ L"Opened dab-lib README" }
                : hstring{ L"Windows declined to open the dab-lib README" });
        }
        catch (hresult_error const& error)
        {
            AddTimelineEvent(L"Unable to open the dab-lib README: " + error.message());
        }
    }

    void MainWindow::HelpAbout_Click(
        IInspectable const&,
        RoutedEventArgs const&)
    {
        std::wstring content =
            L"DAB+ Measurement Workbench\r\n"
            L"Development measurement application from dab-lib\r\n\r\n"
            L"Runtime: Windows App SDK 1.8 / C++ WinUI 3\r\n"
            L"Architecture: x64\r\n"
            L"Receiver pipeline: DAB Mode I, 2.048 MS/s\r\n"
            L"Supported front ends: SDRplay and RTL-SDR\r\n\r\n"
            L"Current device: ";
        content += StatusDeviceText().Text().c_str();
        content += L"\r\nCurrent channel: ";
        content += StatusChannelText().Text().c_str();
        content += L" / ";
        content += FrequencyTextBox().Text().c_str();
        content += L"\r\nReceiver state: ";
        content += StatusSyncText().Text().c_str();
        content += L"\r\n\r\n"
            L"This is an engineering measurement build. Absolute RF power requires a valid calibration profile. "
            L"Unavailable commands are disabled until their backing capture, replay, configuration, or online service exists.";
        ShowHelpDialog(
            L"About DAB+ Measurement Workbench",
            hstring{ content });
    }

    void MainWindow::DabOverviewKpi_Click(IInspectable const& sender, RoutedEventArgs const&)
    {
        auto const button = sender.try_as<Button>();
        if (!button || !button.Tag())
        {
            return;
        }

        hstring const metric = unbox_value<hstring>(button.Tag());
        if (metric == L"fic:crc")
        {
            DabTabView().SelectedItem(DabFicTab());
            FicTabView().SelectedItem(FicFibTimelineTab());
            DabFicFocusText().Text(L"Selected metric: FIC CRC");
        }
        else if (metric == L"fic:fig")
        {
            DabTabView().SelectedItem(DabFicTab());
            FicTabView().SelectedItem(FicFigBrowserTab());
            DabFicFocusText().Text(L"Selected metric: FIG parsing");
        }
        else if (metric == L"fic:reconfiguration")
        {
            DabTabView().SelectedItem(DabFicTab());
            FicTabView().SelectedItem(FicDatabaseTab());
            DabFicFocusText().Text(L"Selected metric: FIC database reconfiguration");
        }
        else if (metric == L"msc:ber")
        {
            DabTabView().SelectedItem(DabMscTab());
            MscTabView().SelectedItem(MscErrorTimelineTab());
            DabMscFocusText().Text(L"Selected metric: MSC post-Viterbi BER");
        }
        else if (metric == L"msc:service")
        {
            DabTabView().SelectedItem(DabMscTab());
            MscTabView().SelectedItem(MscSubchannelsTab());
            DabMscFocusText().Text(L"Selected metric: selected service and protection");
        }
        else
        {
            DabTabView().SelectedItem(DabPlusTab());
            if (metric == L"dab:rs")
            {
                DabPlusFocusText().Text(L"Selected metric: Reed–Solomon corrections");
            }
            else if (metric == L"dab:availability")
            {
                DabPlusFocusText().Text(L"Selected metric: audio superframe availability");
            }
            else
            {
                DabPlusFocusText().Text(L"Selected metric: audio-unit CRC");
            }
        }
    }

    void MainWindow::FicMetricCard_Click(IInspectable const& sender,
                                          RoutedEventArgs const&)
    {
        auto const button = sender.try_as<Button>();
        if (!button || !button.Tag())
        {
            return;
        }
        hstring const metric = unbox_value<hstring>(button.Tag());
        std::wstring_view const metric_view{ metric.c_str(), metric.size() };
        if (metric_view.starts_with(L"fib:"))
        {
            FicTabView().SelectedItem(FicFibTimelineTab());
        }
        else if (metric_view.starts_with(L"fig:"))
        {
            FicTabView().SelectedItem(FicFigBrowserTab());
        }
        else if (metric_view.starts_with(L"database:"))
        {
            FicTabView().SelectedItem(FicDatabaseTab());
        }
        else
        {
            FicTabView().SelectedItem(FicOverviewTab());
        }
        DabFicFocusText().Text(L"Selected metric: " + metric);
    }

    void MainWindow::FicTimelineResume_Click(IInspectable const&,
                                              RoutedEventArgs const&)
    {
        m_fic_timeline_frozen = false;
        FicTimelineResumeButton().Content(box_value(L"Live"));
        FicTabView().SelectedItem(FicFibTimelineTab());
        FicRawCaptureStateText().Text(L"Live timeline");
        UpdateFicTimelineVisuals();
        RefreshFicFailureList();
    }

    void MainWindow::FicFailureListView_ItemClick(
        IInspectable const&,
        ItemClickEventArgs const& args)
    {
        auto const row = args.ClickedItem().try_as<FrameworkElement>();
        if (!row || !row.Tag())
        {
            return;
        }
        size_t const index = static_cast<size_t>(
            unbox_value<uint64_t>(row.Tag()));
        if (index >= m_fic_fib_history.size())
        {
            return;
        }
        m_fic_timeline_frozen = true;
        FicTimelineResumeButton().Content(box_value(L"Frozen"));
        InspectFicHistoryEntry(index);
        FicTabView().SelectedItem(FicRawInspectorTab());
    }

    void MainWindow::FicFigFilter_Changed(IInspectable const&,
                                           SelectionChangedEventArgs const&)
    {
        RefreshFicBrowser();
    }

    void MainWindow::FicFigSearch_Changed(IInspectable const&,
                                           TextChangedEventArgs const&)
    {
        RefreshFicBrowser();
    }

    void MainWindow::FicFigChangedOnly_Click(IInspectable const&,
                                              RoutedEventArgs const&)
    {
        RefreshFicBrowser();
    }

    void MainWindow::FicFigListView_SelectionChanged(
        IInspectable const&,
        SelectionChangedEventArgs const&)
    {
        auto const row = FicFigListView().SelectedItem().try_as<FrameworkElement>();
        if (!row || !row.Tag())
        {
            return;
        }
        m_fic_selected_fig_index = unbox_value<uint32_t>(row.Tag());
        UpdateSelectedFig();
    }

    void MainWindow::FicFreezeOnChange_Click(IInspectable const&,
                                              RoutedEventArgs const&)
    {
        if (!FicFigBrowserStatusText())
        {
            return;
        }
        bool const enabled = FicFreezeOnChangeCheckBox().IsChecked() &&
                             FicFreezeOnChangeCheckBox().IsChecked().Value();
        if (!enabled && m_fic_browser_frozen)
        {
            m_fic_browser_frozen = false;
            FicFigBrowserStatusText().Text(L"LIVE");
            AddTimelineEvent(L"FIG browser resumed");
        }
        else
        {
            FicFigBrowserStatusText().Text(
                enabled ? L"LIVE · freeze on change armed" : L"LIVE");
        }
    }

    void MainWindow::FicCaptureIqOnChange_Click(IInspectable const&,
                                                 RoutedEventArgs const&)
    {
        if (!FicFigBrowserStatusText())
        {
            return;
        }
        bool const armed = FicCaptureIqOnChangeCheckBox().IsChecked() &&
                           FicCaptureIqOnChangeCheckBox().IsChecked().Value();
        FicFigBrowserStatusText().Text(
            armed ? L"ARMED · latest null-scope IQ will be captured on change" : L"LIVE");
        AddTimelineEvent(armed
            ? L"FIC configuration-change IQ capture armed"
            : L"FIC configuration-change IQ capture disarmed");
    }

    void MainWindow::FicCopySelectedFig_Click(IInspectable const&,
                                               RoutedEventArgs const&)
    {
        if (m_fic_selected_fig_index >= m_fic_fig_stat_count)
        {
            FicFigBrowserStatusText().Text(L"Select a FIG first");
            return;
        }
        using namespace Windows::ApplicationModel::DataTransfer;
        DataPackage package;
        package.SetText(FicFigSelectedTitleText().Text() + L"\r\n" +
                        FicFigSelectedDecodedText().Text() + L"\r\n" +
                        FicFigSelectedRawText().Text());
        Clipboard::SetContent(package);
        FicFigBrowserStatusText().Text(L"Selected FIG copied");
    }

    winrt::fire_and_forget MainWindow::FicExportSelectedFig_Click(
        IInspectable const&,
        RoutedEventArgs const&)
    {
        auto lifetime = get_strong();
        if (m_fic_selected_fig_index >= m_fic_fig_stat_count)
        {
            FicFigBrowserStatusText().Text(L"Select a FIG first");
            co_return;
        }
        try
        {
            auto const& stat = m_fic_fig_stats[m_fic_selected_fig_index];
            std::wstring text = FicFigSelectedTitleText().Text().c_str();
            text += L"\r\n";
            text += FicFigSelectedDecodedText().Text().c_str();
            text += L"\r\nCurrent: ";
            text += FicFigSelectedRawText().Text().c_str();
            text += L"\r\nPrevious: ";
            text += FicFigSelectedPreviousText().Text().c_str();
            text += L"\r\n";

            using namespace Windows::Storage;
            SYSTEMTIME now{};
            GetLocalTime(&now);
            wchar_t filename[96]{};
            swprintf_s(filename,
                       L"DAB_FIG_%u_%u_%04u%02u%02u_%02u%02u%02u.txt",
                       stat.type,
                       stat.extension,
                       now.wYear,
                       now.wMonth,
                       now.wDay,
                       now.wHour,
                       now.wMinute,
                       now.wSecond);
            auto const file = co_await DownloadsFolder::CreateFileAsync(
                filename,
                CreationCollisionOption::GenerateUniqueName);
            co_await FileIO::WriteTextAsync(file, hstring{ text });
            FicFigBrowserStatusText().Text(L"Exported · " + file.Path());
            AddTimelineEvent(L"FIC FIG exported to " + file.Path());
        }
        catch (hresult_error const& error)
        {
            FicFigBrowserStatusText().Text(L"Export failed · " + error.message());
        }
    }

    void MainWindow::MscMetricCard_Click(IInspectable const& sender,
                                          RoutedEventArgs const&)
    {
        auto const button = sender.try_as<Button>();
        if (!button || !button.Tag())
        {
            return;
        }
        std::wstring_view const metric{ unbox_value<hstring>(button.Tag()) };
        if (metric.starts_with(L"timeline:"))
        {
            MscTabView().SelectedItem(MscErrorTimelineTab());
        }
        else if (metric.starts_with(L"buffers:"))
        {
            MscTabView().SelectedItem(MscBuffersTab());
        }
        else
        {
            MscTabView().SelectedItem(MscRawDataTab());
        }
    }

    void MainWindow::MscCapacitySegment_Click(IInspectable const& sender,
                                               RoutedEventArgs const&)
    {
        auto const element = sender.try_as<FrameworkElement>();
        if (element && element.Tag())
        {
            SelectMscSubchannel(
                static_cast<uint8_t>(unbox_value<uint32_t>(element.Tag())));
        }
    }

    void MainWindow::MscCapacitySegment_DoubleTapped(
        IInspectable const& sender,
        DoubleTappedRoutedEventArgs const& args)
    {
        auto const element = sender.try_as<FrameworkElement>();
        if (!element || !element.Tag())
        {
            return;
        }
        uint8_t const subchannel_id =
            static_cast<uint8_t>(unbox_value<uint32_t>(element.Tag()));
        SelectMscSubchannel(subchannel_id);
        dab_receive_component_info_t const* component = nullptr;
        auto const* service =
            FindMscServiceForSubchannel(subchannel_id, &component);
        if (service != nullptr)
        {
            SelectAudioService(service->service_id);
            DabTabView().SelectedItem(DabPlusTab());
            AddTimelineEvent(
                L"MSC capacity map selected service " +
                fic_label_text(service->label));
        }
        args.Handled(true);
    }

    void MainWindow::MscSubchannelListView_ItemClick(
        IInspectable const&,
        ItemClickEventArgs const& args)
    {
        auto const row = args.ClickedItem().try_as<FrameworkElement>();
        if (row && row.Tag())
        {
            SelectMscSubchannel(
                static_cast<uint8_t>(unbox_value<uint32_t>(row.Tag())));
        }
    }

    void MainWindow::DabPlusMetricCard_Click(
        IInspectable const& sender,
        RoutedEventArgs const&)
    {
        auto const button = sender.try_as<Button>();
        if (!button)
        {
            return;
        }
        hstring const tag = unbox_value_or<hstring>(button.Tag(), L"");
        if (tag == L"Superframes")
        {
            DabPlusMeasurementTabView().SelectedIndex(1);
        }
        else if (tag == L"ReedSolomon")
        {
            DabPlusMeasurementTabView().SelectedIndex(2);
        }
        else if (tag == L"AudioUnits")
        {
            DabPlusMeasurementTabView().SelectedIndex(3);
        }
        else if (tag == L"Audio")
        {
            DabPlusMeasurementTabView().SelectedIndex(4);
        }
    }

    void MainWindow::DabPlusSuperframeSelectionChanged(
        IInspectable const&,
        SelectionChangedEventArgs const&)
    {
        UpdateDabPlusSelectedSuperframe();
    }

    void MainWindow::DabPlusAudioUnitSelectionChanged(
        IInspectable const&,
        SelectionChangedEventArgs const&)
    {
        UpdateDabPlusSelectedAudioUnit();
    }

    void MainWindow::MscCompareCapacity_Click(IInspectable const&,
                                               RoutedEventArgs const&)
    {
        RenderMscCapacityMap();
    }

    winrt::fire_and_forget MainWindow::MscExportCapacity_Click(
        IInspectable const&,
        RoutedEventArgs const&)
    {
        auto lifetime = get_strong();
        if (!m_have_catalog)
        {
            MscCapacityConfigSummaryText().Text(
                L"Export unavailable · no multiplex database");
            co_return;
        }
        try
        {
            std::wstring csv =
                L"subchannel,service_id,service,start_cu,size_cu,bitrate_kbps,"
                L"protection,allocation_valid,overlap\r\n";
            for (uint32_t index = 0u;
                 index < DAB_RECEIVE_CATALOG_MAX_SUBCHANNELS;
                 ++index)
            {
                auto const& subchannel = m_latest_catalog.subchannels[index];
                if (subchannel.valid == 0u)
                {
                    continue;
                }
                dab_receive_component_info_t const* component = nullptr;
                auto const* service = FindMscServiceForSubchannel(
                    subchannel.subchannel_id,
                    &component);
                hstring const label = service != nullptr
                    ? fic_label_text(service->label)
                    : hstring{ L"Unreferenced" };
                wchar_t row[512]{};
                swprintf_s(row,
                           L"%u,0x%08X,\"%s\",%u,%u,%u,\"%s\",%s,%s\r\n",
                           subchannel.subchannel_id,
                           service != nullptr ? service->service_id : 0u,
                           label.c_str(),
                           subchannel.start_cu,
                           subchannel.size_cu,
                           subchannel.bitrate_kbps,
                           msc_protection_text(subchannel).c_str(),
                           subchannel.allocation_valid != 0u ? L"true" : L"false",
                           subchannel.overlaps != 0u ? L"true" : L"false");
                csv += row;
            }

            using namespace Windows::Storage;
            SYSTEMTIME now{};
            GetLocalTime(&now);
            wchar_t filename[96]{};
            swprintf_s(filename,
                       L"DAB_MSC_Allocation_%04u%02u%02u_%02u%02u%02u.csv",
                       now.wYear,
                       now.wMonth,
                       now.wDay,
                       now.wHour,
                       now.wMinute,
                       now.wSecond);
            auto const file = co_await DownloadsFolder::CreateFileAsync(
                filename,
                CreationCollisionOption::GenerateUniqueName);
            co_await FileIO::WriteTextAsync(file, hstring{ csv });
            MscCapacityConfigSummaryText().Text(
                L"Exported allocation · " + file.Path());
            AddTimelineEvent(L"MSC allocation exported to " + file.Path());
        }
        catch (hresult_error const& error)
        {
            MscCapacityConfigSummaryText().Text(
                L"Allocation export failed · " + error.message());
        }
    }

    void MainWindow::MscTimelineOverlay_Click(IInspectable const&,
                                               RoutedEventArgs const&)
    {
        if (MscTimelineCanvas())
        {
            UpdateMscTimeline();
        }
    }

    void MainWindow::RootLayout_SizeChanged(IInspectable const&, SizeChangedEventArgs const& args)
    {
        ApplyWorkspaceWidth(static_cast<double>(args.NewSize().Width));
    }

    void MainWindow::ApplyWorkspaceWidth(double root_width)
    {
        double const workspace_width = (std::max)(520.0, root_width - 150.0);
        if (ReceiverWorkspaceMain())
        {
            ReceiverWorkspaceMain().Width(m_cir_panel_maximized
                ? (std::max)(520.0, root_width - 20.0)
                : workspace_width);
        }
        if (ReceiverTimelinePanel())
        {
            ReceiverTimelinePanel().Width(workspace_width);
        }
        if (RfWorkspace())
        {
            RfWorkspace().Width(workspace_width);
        }
        if (DabWorkspace())
        {
            DabWorkspace().Width(workspace_width);
        }
        if (RecordingReplayWorkspace())
        {
            RecordingReplayWorkspace().Width(workspace_width);
        }
        if (RfOverviewContent())
        {
            RfOverviewContent().Width((std::max)(480.0, workspace_width - 36.0));
        }
        if (RfNullScopeContent())
        {
            RfNullScopeContent().Width((std::max)(480.0, workspace_width - 36.0));
        }
    }

    void MainWindow::RfWorkspace_SizeChanged(IInspectable const&, SizeChangedEventArgs const& args)
    {
        if (RfOverviewContent() && args.NewSize().Width > 48.0f)
        {
            RfOverviewContent().Width(static_cast<double>(args.NewSize().Width) - 36.0);
        }
        if (RfNullScopeContent() && args.NewSize().Width > 48.0f)
        {
            RfNullScopeContent().Width(static_cast<double>(args.NewSize().Width) - 36.0);
        }
    }

    void MainWindow::CirExpandButton_Click(IInspectable const&, RoutedEventArgs const&)
    {
        SetCirPanelMaximized(!m_cir_panel_maximized);
    }

    void MainWindow::SetCirPanelMaximized(bool maximized)
    {
        if (m_cir_panel_maximized == maximized)
        {
            return;
        }
        m_cir_panel_maximized = maximized;

        WorkspaceNavigationPanel().Visibility(maximized ? Visibility::Collapsed : Visibility::Visible);
        ReceiverCatalogPanel().Visibility(maximized ? Visibility::Collapsed : Visibility::Visible);
        ReceiverSpectrumPanel().Visibility(maximized ? Visibility::Collapsed : Visibility::Visible);
        ReceiverKpiPanel().Visibility(maximized ? Visibility::Collapsed : Visibility::Visible);
        ReceiverTimelinePanel().Visibility(maximized
            ? Visibility::Collapsed
            : (m_rf_workspace_active ? Visibility::Collapsed : Visibility::Visible));

        ReceiverWorkspaceMain().Margin(maximized
            ? Thickness{ 10.0, 10.0, 10.0, 10.0 }
            : Thickness{ 140.0, 10.0, 10.0, 10.0 });
        Grid::SetRowSpan(ReceiverWorkspaceMain(), maximized ? 2 : 1);
        Grid::SetColumn(ReceiverCenterGrid(), maximized ? 0 : 1);
        Grid::SetColumnSpan(ReceiverCenterGrid(), maximized ? 3 : 1);
        Grid::SetRow(ReceiverAnalysisPanel(), maximized ? 0 : 1);
        Grid::SetRowSpan(ReceiverAnalysisPanel(), maximized ? 2 : 1);

        CirExpandIcon().Glyph(maximized ? L"\xE73F" : L"\xE740");
        hstring const accessible_name = maximized
            ? hstring{ L"Restore analysis panel" }
            : hstring{ L"Maximize analysis panel" };
        ToolTipService::SetToolTip(CirExpandButton(), box_value(accessible_name + L" (Ctrl+M)"));
        Microsoft::UI::Xaml::Automation::AutomationProperties::SetName(
            CirExpandButton(),
            accessible_name);
        ApplyWorkspaceWidth(RootLayout().ActualWidth());
        UpdateViewMenuState();
    }

    void MainWindow::RfSpectrum_Loaded(IInspectable const&, RoutedEventArgs const&)
    {
        if (m_rf_spectrum_controls_ready)
        {
            return;
        }

        if (auto const selected_channel = ChannelComboBox().SelectedItem().try_as<ComboBoxItem>())
        {
            auto const tag = unbox_value<hstring>(selected_channel.Tag());
            double const selected_mhz = std::wcstod(tag.c_str(), nullptr);
            if (std::isfinite(selected_mhz) && selected_mhz > 0.0)
            {
                m_rf_spectrum_source_center_hz = selected_mhz * 1000000.0;
                m_rf_spectrum_source_start_hz = m_rf_spectrum_source_center_hz - 1024000.0;
                m_rf_spectrum_view_center_hz = m_rf_spectrum_source_center_hz;
            }
        }
        m_rf_spectrum_controls_ready = true;
        UpdateRfSpectrumReadouts();
        RenderRfSpectrum();
    }

    void MainWindow::RfSpectrumControls_SelectionChanged(IInspectable const& sender,
                                                          SelectionChangedEventArgs const&)
    {
        if (!m_rf_spectrum_controls_ready || m_rf_spectrum_updating_controls)
        {
            return;
        }

        auto const element = sender.try_as<FrameworkElement>();
        if (!element)
        {
            return;
        }

        auto read_selected_tag = [](ComboBox const& combo, double fallback)
        {
            auto const item = combo.SelectedItem().try_as<ComboBoxItem>();
            if (!item || !item.Tag())
            {
                return fallback;
            }
            auto const value = unbox_value<hstring>(item.Tag());
            wchar_t* end = nullptr;
            double const parsed = std::wcstod(value.c_str(), &end);
            return end != value.c_str() && std::isfinite(parsed) ? parsed : fallback;
        };

        if (element.Name() == L"RfSpectrumSpanComboBox")
        {
            m_rf_spectrum_view_span_hz = read_selected_tag(RfSpectrumSpanComboBox(),
                                                           m_rf_spectrum_view_span_hz);
            ClampRfSpectrumView();
            RfSpectrumWaterfallCanvas().Children().Clear();
        }
        else if (element.Name() == L"RfSpectrumAverageComboBox")
        {
            m_rf_spectrum_average_factor = static_cast<uint32_t>(read_selected_tag(
                RfSpectrumAverageComboBox(),
                static_cast<double>(m_rf_spectrum_average_factor)));
            m_rf_spectrum_average_initialized = false;
        }
        else if (element.Name() == L"RfSpectrumScaleComboBox")
        {
            m_rf_spectrum_scale_db = read_selected_tag(RfSpectrumScaleComboBox(),
                                                       m_rf_spectrum_scale_db);
        }

        UpdateRfSpectrumReadouts();
        RenderRfSpectrum();
    }

    void MainWindow::RfSpectrumCenter_ValueChanged(IInspectable const&,
                                                    NumberBoxValueChangedEventArgs const& args)
    {
        if (!m_rf_spectrum_controls_ready || m_rf_spectrum_updating_controls ||
            !std::isfinite(args.NewValue()))
        {
            return;
        }
        m_rf_spectrum_view_center_hz = args.NewValue() * 1000000.0;
        ClampRfSpectrumView();
        RfSpectrumWaterfallCanvas().Children().Clear();
        UpdateRfSpectrumReadouts();
        RenderRfSpectrum();
    }

    void MainWindow::RfSpectrumReference_ValueChanged(IInspectable const&,
                                                       NumberBoxValueChangedEventArgs const& args)
    {
        if (!m_rf_spectrum_controls_ready || m_rf_spectrum_updating_controls ||
            !std::isfinite(args.NewValue()))
        {
            return;
        }
        m_rf_spectrum_reference_dbfs = args.NewValue();
        UpdateRfSpectrumReadouts();
        RenderRfSpectrum();
    }

    void MainWindow::RfSpectrumMarker_Click(IInspectable const& sender, RoutedEventArgs const&)
    {
        auto const button = sender.try_as<ToggleButton>();
        if (!button)
        {
            return;
        }
        m_rf_spectrum_active_marker = button.Name() == L"RfSpectrumMarker2Button" ? 1u : 0u;
        RfSpectrumMarker1Button().IsChecked(m_rf_spectrum_active_marker == 0u);
        RfSpectrumMarker2Button().IsChecked(m_rf_spectrum_active_marker == 1u);
        UpdateRfSpectrumReadouts();
    }

    void MainWindow::RfSpectrumClearPeaks_Click(IInspectable const&, RoutedEventArgs const&)
    {
        if (m_rf_spectrum_count != 0u)
        {
            std::copy_n(m_rf_spectrum_average.begin(),
                        m_rf_spectrum_count,
                        m_rf_spectrum_peak.begin());
        }
        RenderRfSpectrum();
        AddTimelineEvent(L"Spectrum peak hold cleared");
    }

    void MainWindow::RfSpectrumPeakHold_Click(IInspectable const&, RoutedEventArgs const&)
    {
        auto const checked = RfSpectrumPeakHoldButton().IsChecked();
        m_rf_spectrum_peak_hold = checked && checked.Value();
        if (!m_rf_spectrum_peak_hold && m_rf_spectrum_count != 0u)
        {
            std::copy_n(m_rf_spectrum_average.begin(),
                        m_rf_spectrum_count,
                        m_rf_spectrum_peak.begin());
        }
        RenderRfSpectrum();
    }

    void MainWindow::RfSpectrumFreeze_Click(IInspectable const&, RoutedEventArgs const&)
    {
        auto const checked = RfSpectrumFreezeButton().IsChecked();
        m_rf_spectrum_frozen = checked && checked.Value();
        RfSpectrumInteractionHintText().Text(m_rf_spectrum_frozen
            ? L"Display frozen · interaction remains available"
            : L"Wheel zoom · drag pan · double-click reset · click/drag marker");
        AddTimelineEvent(m_rf_spectrum_frozen
            ? hstring{ L"Spectrum display frozen" }
            : hstring{ L"Spectrum display resumed" });
    }

    void MainWindow::RfSpectrumCanvas_PointerWheelChanged(IInspectable const&,
                                                           PointerRoutedEventArgs const& args)
    {
        auto const point = args.GetCurrentPoint(RfSpectrumCanvas());
        auto const position = point.Position();
        m_rf_spectrum_cursor_frequency_hz = RfSpectrumFrequencyFromX(position.X);
        double const anchor = m_rf_spectrum_cursor_frequency_hz;
        double const ratio = std::clamp(static_cast<double>(position.X) / 1000.0, 0.0, 1.0);
        double const old_span = m_rf_spectrum_view_span_hz;
        double const zoom = point.Properties().MouseWheelDelta() > 0 ? 0.80 : 1.25;
        double const maximum_span = m_rf_spectrum_sample_rate_hz != 0u
            ? static_cast<double>(m_rf_spectrum_sample_rate_hz)
            : 2048000.0;
        m_rf_spectrum_view_span_hz = std::clamp(old_span * zoom, 20000.0, maximum_span);
        m_rf_spectrum_view_center_hz = anchor - (ratio - 0.5) * m_rf_spectrum_view_span_hz;
        ClampRfSpectrumView();
        RfSpectrumWaterfallCanvas().Children().Clear();
        UpdateRfSpectrumReadouts();
        RenderRfSpectrum();
        UpdateRfSpectrumHover(position);
        args.Handled(true);
    }

    void MainWindow::RfSpectrumCanvas_PointerPressed(IInspectable const&,
                                                      PointerRoutedEventArgs const& args)
    {
        auto const point = args.GetCurrentPoint(RfSpectrumCanvas());
        if (!point.Properties().IsLeftButtonPressed())
        {
            m_rf_spectrum_cursor_frequency_hz = RfSpectrumFrequencyFromX(point.Position().X);
            UpdateRfSpectrumHover(point.Position());
            return;
        }

        m_rf_spectrum_pointer_pressed = true;
        m_rf_spectrum_pointer_moved = false;
        m_rf_spectrum_pointer_start = point.Position();
        m_rf_spectrum_pan_start_center_hz = m_rf_spectrum_view_center_hz;
        m_rf_spectrum_drag_marker = -1;
        for (uint32_t marker = 0u; marker < 2u; ++marker)
        {
            if (m_rf_spectrum_marker_valid[marker] &&
                std::abs(RfSpectrumXFromFrequency(m_rf_spectrum_marker_frequency_hz[marker]) -
                         point.Position().X) <= 10.0)
            {
                m_rf_spectrum_drag_marker = static_cast<int32_t>(marker);
                m_rf_spectrum_active_marker = marker;
                break;
            }
        }
        RfSpectrumCanvas().CapturePointer(args.Pointer());
        args.Handled(true);
    }

    void MainWindow::RfSpectrumCanvas_PointerMoved(IInspectable const&,
                                                    PointerRoutedEventArgs const& args)
    {
        auto const position = args.GetCurrentPoint(RfSpectrumCanvas()).Position();
        if (m_rf_spectrum_pointer_pressed)
        {
            double const delta_x = static_cast<double>(position.X - m_rf_spectrum_pointer_start.X);
            if (std::abs(delta_x) > 3.0)
            {
                m_rf_spectrum_pointer_moved = true;
            }
            if (m_rf_spectrum_drag_marker >= 0)
            {
                SetRfSpectrumMarker(static_cast<uint32_t>(m_rf_spectrum_drag_marker),
                                    RfSpectrumFrequencyFromX(position.X));
            }
            else if (m_rf_spectrum_pointer_moved)
            {
                m_rf_spectrum_view_center_hz = m_rf_spectrum_pan_start_center_hz -
                    delta_x * m_rf_spectrum_view_span_hz / 1000.0;
                ClampRfSpectrumView();
                UpdateRfSpectrumReadouts();
                RenderRfSpectrum();
            }
        }
        UpdateRfSpectrumHover(position);
        args.Handled(true);
    }

    void MainWindow::RfSpectrumCanvas_PointerReleased(IInspectable const&,
                                                       PointerRoutedEventArgs const& args)
    {
        auto const position = args.GetCurrentPoint(RfSpectrumCanvas()).Position();
        if (m_rf_spectrum_pointer_pressed && !m_rf_spectrum_pointer_moved)
        {
            SetRfSpectrumMarker(m_rf_spectrum_active_marker,
                                RfSpectrumFrequencyFromX(position.X));
        }
        if (m_rf_spectrum_pointer_moved && m_rf_spectrum_drag_marker < 0)
        {
            RfSpectrumWaterfallCanvas().Children().Clear();
        }
        RfSpectrumCanvas().ReleasePointerCapture(args.Pointer());
        m_rf_spectrum_pointer_pressed = false;
        m_rf_spectrum_pointer_moved = false;
        m_rf_spectrum_drag_marker = -1;
        args.Handled(true);
    }

    void MainWindow::RfSpectrumCanvas_PointerExited(IInspectable const&,
                                                     PointerRoutedEventArgs const&)
    {
        if (!m_rf_spectrum_pointer_pressed)
        {
            RfSpectrumTooltipBorder().Visibility(Visibility::Collapsed);
        }
    }

    void MainWindow::RfSpectrumCanvas_PointerCaptureLost(IInspectable const&,
                                                          PointerRoutedEventArgs const&)
    {
        m_rf_spectrum_pointer_pressed = false;
        m_rf_spectrum_pointer_moved = false;
        m_rf_spectrum_drag_marker = -1;
    }

    void MainWindow::RfSpectrumCanvas_DoubleTapped(IInspectable const&,
                                                    DoubleTappedRoutedEventArgs const& args)
    {
        ResetRfSpectrumView();
        args.Handled(true);
    }

    void MainWindow::RfSpectrumResetView_Click(IInspectable const&, RoutedEventArgs const&)
    {
        ResetRfSpectrumView();
    }

    void MainWindow::RfSpectrumTuneToCursor_Click(IInspectable const&, RoutedEventArgs const&)
    {
        double const frequency_hz = std::clamp(
            m_rf_spectrum_cursor_frequency_hz,
            m_rf_spectrum_source_start_hz,
            m_rf_spectrum_source_start_hz +
                static_cast<double>((std::max)(m_rf_spectrum_count, 1u) - 1u) * m_rf_spectrum_bin_hz);
        wchar_t frequency_text[48]{};
        swprintf_s(frequency_text, L"%.6f MHz", frequency_hz / 1000000.0);
        FrequencyTextBox().Text(frequency_text);
        ChannelComboBox().SelectedIndex(-1);
        StatusChannelText().Text(L"custom");
        m_selected_service_id = 0u;
        UpdateRfContext();
        UpdateDabContext();

        if (!m_receiver.IsActive())
        {
            AddTimelineEvent(hstring{ L"Spectrum tune selected " } + frequency_text +
                             L" — press Start to receive");
            return;
        }

        auto settings = ReadReceiverSettings();
        if (!settings)
        {
            return;
        }
        settings->service_id = 0u;
        m_pending_receiver_restart = *settings;
        ClearCatalogView();
        m_receiver.RequestStop();
        StartButton().IsEnabled(false);
        StartButtonLabel().Text(L"Retuning...");
        DataSeamText().Text(hstring{ L"retuning to " } + frequency_text);
        AddTimelineEvent(hstring{ L"Retuning receiver from spectrum to " } + frequency_text);
    }

    winrt::fire_and_forget MainWindow::RfSpectrumSaveScreenshot_Click(
        IInspectable const&,
        RoutedEventArgs const&)
    {
        auto lifetime = get_strong();
        try
        {
            using namespace Windows::Graphics::Imaging;
            using namespace Windows::Storage;
            using namespace Windows::Storage::Streams;
            using namespace Microsoft::UI::Xaml::Media::Imaging;

            RenderTargetBitmap bitmap;
            co_await bitmap.RenderAsync(RfSpectrumCaptureRoot());
            auto const pixel_buffer = co_await bitmap.GetPixelsAsync();
            DataReader reader = DataReader::FromBuffer(pixel_buffer);
            std::vector<uint8_t> pixels(pixel_buffer.Length());
            reader.ReadBytes(pixels);

            SYSTEMTIME now{};
            GetLocalTime(&now);
            wchar_t filename[80]{};
            swprintf_s(filename,
                       L"DAB_Spectrum_%04u%02u%02u_%02u%02u%02u.png",
                       now.wYear,
                       now.wMonth,
                       now.wDay,
                       now.wHour,
                       now.wMinute,
                       now.wSecond);
            auto const file = co_await KnownFolders::PicturesLibrary().CreateFileAsync(
                filename,
                CreationCollisionOption::GenerateUniqueName);
            auto const stream = co_await file.OpenAsync(FileAccessMode::ReadWrite);
            auto const encoder = co_await BitmapEncoder::CreateAsync(BitmapEncoder::PngEncoderId(), stream);
            encoder.SetPixelData(BitmapPixelFormat::Bgra8,
                                 BitmapAlphaMode::Premultiplied,
                                 static_cast<uint32_t>(bitmap.PixelWidth()),
                                 static_cast<uint32_t>(bitmap.PixelHeight()),
                                 96.0,
                                 96.0,
                                 pixels);
            co_await encoder.FlushAsync();
            AddTimelineEvent(L"Spectrum screenshot saved to " + file.Path());
        }
        catch (hresult_error const& error)
        {
            AddTimelineEvent(L"Spectrum screenshot failed: " + error.message());
        }
    }

    void MainWindow::RfNullScope_Loaded(IInspectable const&, RoutedEventArgs const&)
    {
        m_rf_null_controls_ready = true;
        RenderRfNullScope();
        UpdateRfNullScopeReadouts();
    }

    void MainWindow::RfNullControls_SelectionChanged(IInspectable const&,
                                                      SelectionChangedEventArgs const&)
    {
        if (!m_rf_null_controls_ready)
        {
            return;
        }
        auto read_tag = [](ComboBox const& box, double fallback)
        {
            auto const item = box.SelectedItem().try_as<ComboBoxItem>();
            if (!item || !item.Tag())
            {
                return fallback;
            }
            std::wstring const text{ unbox_value<hstring>(item.Tag()).c_str() };
            wchar_t* end = nullptr;
            double const value = std::wcstod(text.c_str(), &end);
            return end != text.c_str() && std::isfinite(value) ? value : fallback;
        };
        m_rf_null_time_div_us = read_tag(RfNullTimebaseComboBox(), 500.0);
        m_rf_null_vertical_db_div = read_tag(RfNullVerticalComboBox(), 10.0);
        m_rf_null_average_factor = static_cast<uint32_t>(std::clamp(
            read_tag(RfNullAverageComboBox(), 16.0),
            1.0,
            32.0));
        m_rf_null_filtered_source = RfNullSourceComboBox().SelectedIndex() == 0;
        RenderRfNullScope();
        UpdateRfNullScopeReadouts();
    }

    void MainWindow::RfNullNumber_ValueChanged(IInspectable const&,
                                                NumberBoxValueChangedEventArgs const&)
    {
        if (!m_rf_null_controls_ready)
        {
            return;
        }
        double position = RfNullPositionNumberBox().Value();
        double holdoff = RfNullHoldoffNumberBox().Value();
        m_rf_null_trigger_position_percent = std::isfinite(position)
            ? std::clamp(position, 0.0, 80.0)
            : 20.0;
        m_rf_null_holdoff_ms = std::isfinite(holdoff)
            ? std::clamp(holdoff, 0.0, 1000.0)
            : 90.0;
        RenderRfNullScope();
        UpdateRfNullScopeReadouts();
    }

    void MainWindow::RfNullRun_Click(IInspectable const&, RoutedEventArgs const&)
    {
        m_rf_null_frozen = false;
        m_rf_null_single_armed = false;
        RfNullFreezeButton().IsChecked(false);
        RfNullControlStatusText().Text(L"Live acquisition");
        RfNullLiveText().Text(L"LIVE");
    }

    void MainWindow::RfNullSingle_Click(IInspectable const&, RoutedEventArgs const&)
    {
        m_rf_null_single_armed = true;
        m_rf_null_frozen = false;
        RfNullFreezeButton().IsChecked(false);
        RfNullControlStatusText().Text(L"Single capture armed");
        RfNullLiveText().Text(L"ARMED");
    }

    void MainWindow::RfNullFreeze_Click(IInspectable const&, RoutedEventArgs const&)
    {
        auto const checked = RfNullFreezeButton().IsChecked();
        m_rf_null_frozen = checked && checked.Value();
        m_rf_null_single_armed = false;
        RfNullControlStatusText().Text(m_rf_null_frozen ? L"Display frozen" : L"Live acquisition");
        RfNullLiveText().Text(m_rf_null_frozen ? L"FROZEN" : L"LIVE");
    }

    void MainWindow::RfNullPersistence_Click(IInspectable const&, RoutedEventArgs const&)
    {
        auto const checked = RfNullPersistenceButton().IsChecked();
        m_rf_null_persistence_enabled = checked && checked.Value();
        RenderRfNullScope();
    }

    void MainWindow::RfNullCursors_Click(IInspectable const&, RoutedEventArgs const&)
    {
        auto const checked = RfNullCursorsButton().IsChecked();
        m_rf_null_cursors_enabled = checked && checked.Value();
        RenderRfNullScope();
        UpdateRfNullScopeReadouts();
    }

    void MainWindow::RfNullScopeCanvas_PointerPressed(IInspectable const&,
                                                       PointerRoutedEventArgs const& args)
    {
        if (!m_rf_null_cursors_enabled || m_rf_null_focus_count == 0u)
        {
            return;
        }
        Point const position = args.GetCurrentPoint(RfNullScopeCanvas()).Position();
        constexpr double plot_left = 65.0;
        constexpr double plot_width = 905.0;
        if (position.X < plot_left || position.X > plot_left + plot_width)
        {
            return;
        }
        double span_ms;
        double start_ms;
        if (m_rf_null_time_div_us == 0.0)
        {
            span_ms = m_rf_null_sample_rate_hz != 0u
                ? 1000.0 * static_cast<double>(m_rf_null_frame_sample_count) /
                    static_cast<double>(m_rf_null_sample_rate_hz)
                : 96.0;
            start_ms = 0.0;
        }
        else
        {
            span_ms = 7.0 * m_rf_null_time_div_us / 1000.0;
            start_ms = m_rf_null_time_div_us >= 10000.0
                ? 0.0
                : -span_ms * m_rf_null_trigger_position_percent / 100.0;
        }
        m_rf_null_cursor_ms[m_rf_null_active_cursor] = start_ms +
            span_ms * (position.X - plot_left) / plot_width;
        m_rf_null_active_cursor = (m_rf_null_active_cursor + 1u) % 2u;
        RenderRfNullScope();
        UpdateRfNullScopeReadouts();
        args.Handled(true);
    }

    winrt::fire_and_forget MainWindow::RfNullCaptureIq_Click(
        IInspectable const&,
        RoutedEventArgs const&)
    {
        auto lifetime = get_strong();
        bool const fic_configuration_capture = m_fic_capture_in_progress;
        if (m_rf_null_focus_count == 0u)
        {
            RfNullCaptureStatusText().Text(L"No triggered IQ capture available");
            if (fic_configuration_capture)
            {
                FicFigBrowserStatusText().Text(
                    L"Configuration changed · no recent triggered IQ available");
            }
            m_fic_capture_in_progress = false;
            co_return;
        }
        try
        {
            struct CaptureHeader
            {
                char magic[8];
                uint32_t version;
                uint32_t sample_rate_hz;
                uint64_t center_frequency_hz;
                uint64_t frame_sequence;
                int32_t start_offset_samples;
                uint32_t sample_count;
            };
            CaptureHeader header{
                { 'D', 'A', 'B', 'N', 'I', 'Q', '1', '\0' },
                1u,
                m_rf_null_sample_rate_hz,
                m_rf_null_frequency_hz,
                m_rf_null_sequence,
                m_rf_null_start_offset_samples,
                m_rf_null_focus_count
            };
            size_t const iq_bytes = 2u * static_cast<size_t>(m_rf_null_focus_count) *
                                    sizeof(m_rf_null_latest_iq[0]);
            std::vector<uint8_t> bytes(sizeof(header) + iq_bytes);
            std::memcpy(bytes.data(), &header, sizeof(header));
            std::memcpy(bytes.data() + sizeof(header), m_rf_null_latest_iq.data(), iq_bytes);

            SYSTEMTIME now{};
            GetLocalTime(&now);
            wchar_t filename[96]{};
            swprintf_s(filename,
                       fic_configuration_capture
                           ? L"DAB_FICChange_%04u%02u%02u_%02u%02u%02u_%03u.ci16"
                           : L"DAB_NullScope_%04u%02u%02u_%02u%02u%02u_%03u.ci16",
                       now.wYear,
                       now.wMonth,
                       now.wDay,
                       now.wHour,
                       now.wMinute,
                       now.wSecond,
                       now.wMilliseconds);

            wchar_t local_app_data[MAX_PATH]{};
            DWORD const local_length = GetEnvironmentVariableW(
                L"LOCALAPPDATA",
                local_app_data,
                static_cast<DWORD>(std::size(local_app_data)));
            if (local_length == 0u || local_length >= std::size(local_app_data))
            {
                throw_hresult(HRESULT_FROM_WIN32(GetLastError()));
            }
            std::wstring root{ local_app_data };
            root += L"\\DabMeasurementWinUI";
            if (!CreateDirectoryW(root.c_str(), nullptr) &&
                GetLastError() != ERROR_ALREADY_EXISTS)
            {
                throw_hresult(HRESULT_FROM_WIN32(GetLastError()));
            }
            std::wstring capture_folder = root + L"\\Captures";
            if (!CreateDirectoryW(capture_folder.c_str(), nullptr) &&
                GetLastError() != ERROR_ALREADY_EXISTS)
            {
                throw_hresult(HRESULT_FROM_WIN32(GetLastError()));
            }
            std::wstring const path = capture_folder + L"\\" + filename;
            HANDLE file = CreateFileW(path.c_str(),
                                      GENERIC_WRITE,
                                      FILE_SHARE_READ,
                                      nullptr,
                                      CREATE_NEW,
                                      FILE_ATTRIBUTE_NORMAL,
                                      nullptr);
            if (file == INVALID_HANDLE_VALUE)
            {
                throw_hresult(HRESULT_FROM_WIN32(GetLastError()));
            }
            DWORD written = 0u;
            BOOL const write_ok = WriteFile(file,
                                            bytes.data(),
                                            static_cast<DWORD>(bytes.size()),
                                            &written,
                                            nullptr);
            DWORD const write_error = write_ok && written == bytes.size()
                ? ERROR_SUCCESS
                : (write_ok ? ERROR_WRITE_FAULT : GetLastError());
            CloseHandle(file);
            if (write_error != ERROR_SUCCESS)
            {
                throw_hresult(HRESULT_FROM_WIN32(write_error));
            }
            RfNullCaptureStatusText().Text(L"Saved " + hstring{ filename });
            if (fic_configuration_capture)
            {
                FicFigBrowserStatusText().Text(
                    L"Configuration-change IQ saved · " + hstring{ filename });
                AddTimelineEvent(
                    L"FIC configuration-change IQ capture saved to " +
                    hstring{ path });
            }
            else
            {
                AddTimelineEvent(
                    L"Triggered null IQ capture saved to " + hstring{ path });
            }
        }
        catch (hresult_error const& error)
        {
            RfNullCaptureStatusText().Text(L"IQ capture failed");
            if (fic_configuration_capture)
            {
                FicFigBrowserStatusText().Text(L"Configuration-change IQ capture failed");
                AddTimelineEvent(
                    L"FIC configuration-change IQ capture failed: " +
                    error.message());
            }
            else
            {
                AddTimelineEvent(L"Null IQ capture failed: " + error.message());
            }
        }
        m_fic_capture_in_progress = false;
        co_return;
    }

    void MainWindow::AudioButton_Click(IInspectable const&, RoutedEventArgs const&)
    {
        ShowWorkspace(WorkspaceKind::dab_measurements);
        DabTabView().SelectedIndex(3);
        DabPlusMeasurementTabView().SelectedIndex(4);
        AddTimelineEvent(L"Opened DAB+ Audio measurements");
    }

    void MainWindow::ServicesListView_SelectionChanged(IInspectable const&,
                                                        SelectionChangedEventArgs const&)
    {
        ApplySelectedServiceComponents();
        UpdateDabContext();
    }

    void MainWindow::ServicesListView_ItemClick(IInspectable const&,
                                                ItemClickEventArgs const& args)
    {
        auto const item = args.ClickedItem().try_as<FrameworkElement>();
        if (!item || !item.Tag())
        {
            return;
        }
        SelectAudioService(unbox_value<uint32_t>(item.Tag()));
    }

    void MainWindow::StartButton_Click(IInspectable const&, RoutedEventArgs const&)
    {
        if (m_replay_loaded || m_replay_importing)
        {
            AddTimelineEvent(
                L"Create a new measurement session before starting the live receiver");
            return;
        }
        if (m_receiver.IsActive())
        {
            m_receiver.RequestStop();
            StartButton().IsEnabled(false);
            StartButtonLabel().Text(L"Stopping...");
            AddTimelineEvent(L"Receiver stop requested");
            return;
        }

        auto settings = ReadReceiverSettings();
        if (!settings)
        {
            return;
        }

        StartReceiver(*settings, true, L"Opening real receiver worker");
    }

    void MainWindow::RecordButton_Click(IInspectable const&, RoutedEventArgs const&)
    {
        if (m_replay_loaded || m_replay_importing)
        {
            AddTimelineEvent(L"Replay sessions cannot be recorded");
            return;
        }
        if (m_session_recorder.IsActive())
        {
            StopMeasurementRecording(L"Measurement recording stopped");
            return;
        }

        using dab::measurement::ReceiverState;
        ReceiverState const receiver_state = m_receiver.State();
        if (receiver_state != ReceiverState::running &&
            receiver_state != ReceiverState::recovering)
        {
            AddTimelineEvent(
                L"Recording requires a running receiver");
            return;
        }

        dab::measurement::MeasurementSessionStartInfo info;
        info.recordings_root = measurement_recordings_root();
        info.context = to_string(DabContextText().Text());
        if (auto const selected =
                DeviceComboBox().SelectedItem().try_as<ComboBoxItem>())
        {
            info.device =
                to_string(unbox_value<hstring>(selected.Content()));
        }
        if (auto const selected =
                ChannelComboBox().SelectedItem().try_as<ComboBoxItem>())
        {
            info.channel =
                to_string(unbox_value<hstring>(selected.Content()));
        }
        info.driver = m_have_latest_rf_snapshot
            ? m_latest_rf_snapshot.device_driver
            : (DeviceComboBox().SelectedIndex() == 1
                ? "rtlsdr"
                : "sdrplay");
        info.serial = m_have_latest_rf_snapshot
            ? m_latest_rf_snapshot.device_serial
            : "";
        info.frequency_hz = m_have_latest_rf_snapshot
            ? m_latest_rf_snapshot.frequency_hz
            : 0u;
        info.sample_rate_hz = m_have_latest_rf_snapshot
            ? m_latest_rf_snapshot.sample_rate_hz
            : 2048000u;
        info.gain_reduction_db =
            m_have_latest_rf_snapshot &&
            m_latest_rf_snapshot.applied_gain_valid != 0
                ? m_latest_rf_snapshot.if_gain_reduction_db
                : static_cast<int>(std::lround(GainNumberBox().Value()));
        info.agc_enabled = m_receiver_agc_enabled;
        info.calibration_profile_id =
            m_have_latest_rf_snapshot
                ? m_latest_rf_snapshot.calibration_profile_id
                : "";
        if (m_have_catalog)
        {
            info.ensemble =
                to_string(fic_label_text(m_latest_catalog.ensemble_label));
            for (uint32_t index = 0u;
                 index < m_latest_catalog.service_count;
                 ++index)
            {
                if (m_latest_catalog.services[index].service_id ==
                    m_selected_service_id)
                {
                    info.service = to_string(fic_label_text(
                        m_latest_catalog.services[index].label));
                    break;
                }
            }
        }

        std::string error;
        if (!m_session_recorder.Start(std::move(info), error))
        {
            AddTimelineEvent(
                L"Measurement recording could not start: " +
                to_hstring(error));
            UpdateRecordingUi();
            return;
        }
        m_recording_failure_reported = false;
        if (m_have_latest_rf_snapshot)
        {
            m_session_recorder.RecordSnapshot(m_latest_rf_snapshot);
        }
        if (m_have_catalog)
        {
            m_session_recorder.RecordCatalog(m_latest_catalog);
        }
        auto const status = m_session_recorder.Status();
        AddTimelineEvent(
            L"Measurement recording started · " +
            hstring{ status.session_path.wstring() });
        UpdateRecordingUi();
    }

    void MainWindow::StopMeasurementRecording(hstring const& reason)
    {
        auto const before = m_session_recorder.Status();
        if (!before.active &&
            (!before.failed || m_recording_failure_reported))
        {
            return;
        }
        if (before.active)
        {
            m_session_recorder.RecordEvent(
                0x10001u,
                to_string(reason));
        }
        auto const completed = m_session_recorder.Stop();
        UpdateRecordingUi();

        wchar_t summary[160]{};
        swprintf_s(summary,
                   L" · %.1f MB · %llu measurement frames · %llu dropped",
                   static_cast<double>(completed.bytes_written) /
                       (1024.0 * 1024.0),
                   static_cast<unsigned long long>(
                       completed.measurement_records),
                   static_cast<unsigned long long>(
                       completed.dropped_records));
        AddTimelineEvent(
            reason + summary + L" · " +
            hstring{ completed.session_path.wstring() });
    }

    void MainWindow::UpdateRecordingUi()
    {
        if (!RecordButton())
        {
            return;
        }
        auto status = m_session_recorder.Status();
        if (m_replay_loaded || m_replay_importing)
        {
            RecordButtonLabel().Text(L"Record");
            RecordButton().IsEnabled(false);
            StatusRecordText().Text(
                m_replay_importing ? L"importing recording" : L"replay");
            StatusRecordText().Foreground(
                Application::Current().Resources().Lookup(
                    box_value(hstring{ L"MutedTextBrush" })).as<Brush>());
            return;
        }
        uint64_t const seconds = status.elapsed_ns / 1000000000ull;
        wchar_t duration[32]{};
        swprintf_s(duration,
                   L"%02llu:%02llu:%02llu",
                   static_cast<unsigned long long>(seconds / 3600ull),
                   static_cast<unsigned long long>((seconds / 60ull) % 60ull),
                   static_cast<unsigned long long>(seconds % 60ull));

        if (status.active)
        {
            RecordButtonLabel().Text(hstring{ L"● REC " } + duration);
            RecordButton().Background(
                Application::Current().Resources().Lookup(
                    box_value(hstring{ L"RecordingBrush" })).as<Brush>());
            RecordButton().Foreground(
                SolidColorBrush{ Colors::White() });
            wchar_t status_text_buffer[128]{};
            swprintf_s(status_text_buffer,
                       status.dropped_records == 0u
                           ? L"recording %ls · %.1f MB"
                           : L"recording %ls · %.1f MB · %llu dropped",
                       duration,
                       static_cast<double>(
                           status.bytes_written + status.queued_bytes) /
                           (1024.0 * 1024.0),
                       static_cast<unsigned long long>(
                           status.dropped_records));
            StatusRecordText().Text(status_text_buffer);
            StatusRecordText().Foreground(
                Application::Current().Resources().Lookup(
                    box_value(hstring{ status.dropped_records == 0u
                        ? L"RecordingBrush"
                        : L"WarningBrush" })).as<Brush>());
            ToolTipService::SetToolTip(
                RecordButton(),
                box_value(hstring{ L"Stop recording · " } +
                          hstring{ status.session_path.wstring() }));
        }
        else
        {
            RecordButtonLabel().Text(L"Record");
            RecordButton().Background(
                Application::Current().Resources().Lookup(
                    box_value(hstring{ L"PanelRaisedBrush" })).as<Brush>());
            RecordButton().Foreground(
                SolidColorBrush{ Colors::White() });
            if (status.failed)
            {
                StatusRecordText().Text(
                    hstring{ L"recording error · " } +
                    to_hstring(status.error));
                StatusRecordText().Foreground(
                    Application::Current().Resources().Lookup(
                        box_value(hstring{ L"WarningBrush" })).as<Brush>());
                if (!m_recording_failure_reported)
                {
                    m_recording_failure_reported = true;
                    status = m_session_recorder.Stop();
                    AddTimelineEvent(
                        hstring{ L"Measurement recording failed: " } +
                        to_hstring(status.error) +
                        L" · receiver continues running");
                }
            }
            else
            {
                StatusRecordText().Text(L"not recording");
                StatusRecordText().Foreground(
                    Application::Current().Resources().Lookup(
                        box_value(hstring{ L"MutedTextBrush" })).as<Brush>());
            }
            ToolTipService::SetToolTip(
                RecordButton(),
                box_value(hstring{
                    L"Record snapshots and high-rate measurements; "
                    L"continuous IQ is excluded"
                }));
        }

        using dab::measurement::ReceiverState;
        ReceiverState const receiver_state = m_receiver.State();
        RecordButton().IsEnabled(
            status.active ||
            receiver_state == ReceiverState::running ||
            receiver_state == ReceiverState::recovering);
    }

    void MainWindow::ReplayPlayPause_Click(
        IInspectable const&,
        RoutedEventArgs const&)
    {
        if (!m_replay_loaded)
        {
            return;
        }
        auto const duration = m_session_replay.Info().duration_ns;
        if (!m_replay_playing &&
            m_replay_position_ns >= duration)
        {
            SeekReplay(0u);
        }
        m_replay_playing = !m_replay_playing;
        m_replay_last_tick = std::chrono::steady_clock::now();
        UpdateReplayUi();
    }

    void MainWindow::ReplaySpeed_SelectionChanged(
        IInspectable const&,
        SelectionChangedEventArgs const&)
    {
        if (!ReplaySpeedComboBox())
        {
            return;
        }
        auto const item =
            ReplaySpeedComboBox().SelectedItem().try_as<ComboBoxItem>();
        if (!item || !item.Tag())
        {
            return;
        }
        std::wstring const text{
            unbox_value<hstring>(item.Tag()).c_str()
        };
        wchar_t* end = nullptr;
        double const speed = std::wcstod(text.c_str(), &end);
        if (end != text.c_str() &&
            std::isfinite(speed) &&
            speed > 0.0)
        {
            m_replay_speed = speed;
            m_replay_last_tick = std::chrono::steady_clock::now();
            if (m_replay_loaded)
            {
                UpdateReplayUi();
            }
        }
    }

    void MainWindow::ReplayPosition_ValueChanged(
        IInspectable const&,
        RangeBaseValueChangedEventArgs const& args)
    {
        if (!m_replay_loaded || m_replay_slider_internal)
        {
            return;
        }
        uint64_t const duration = m_session_replay.Info().duration_ns;
        double const fraction = std::clamp(args.NewValue() / 1000.0,
                                           0.0,
                                           1.0);
        uint64_t const target = static_cast<uint64_t>(
            std::llround(fraction * static_cast<double>(duration)));
        SeekReplay(target);
        UpdateReplayUi();
    }

    void MainWindow::AdvanceReplay()
    {
        if (!m_replay_loaded)
        {
            return;
        }

        auto const now = std::chrono::steady_clock::now();
        if (m_replay_last_tick == std::chrono::steady_clock::time_point{})
        {
            m_replay_last_tick = now;
        }
        if (m_replay_playing)
        {
            uint64_t const duration = m_session_replay.Info().duration_ns;
            uint64_t const elapsed_ns = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    now - m_replay_last_tick).count());
            long double const scaled =
                static_cast<long double>(elapsed_ns) *
                static_cast<long double>(m_replay_speed);
            uint64_t const advance = scaled >=
                    static_cast<long double>(
                        (std::numeric_limits<uint64_t>::max)())
                ? (std::numeric_limits<uint64_t>::max)()
                : static_cast<uint64_t>(scaled);
            m_replay_position_ns =
                advance >= duration - (std::min)(duration,
                                                  m_replay_position_ns)
                    ? duration
                    : m_replay_position_ns + advance;

            std::string error;
            for (size_t batch = 0u; batch < 16u; ++batch)
            {
                std::vector<
                    dab::measurement::MeasurementSessionReplayRecord>
                    records;
                if (!m_session_replay.ReadUntil(
                        m_replay_position_ns,
                        records,
                        256u,
                        error))
                {
                    m_replay_playing = false;
                    AddTimelineEvent(
                        L"Replay stopped: " + to_hstring(error));
                    break;
                }
                for (auto const& record : records)
                {
                    if (!ApplyReplayRecord(record, true))
                    {
                        m_replay_playing = false;
                        break;
                    }
                }
                if (!m_replay_playing || records.size() < 256u)
                {
                    break;
                }
            }

            if (m_replay_position_ns >= duration)
            {
                m_replay_playing = false;
            }
        }
        m_replay_last_tick = now;
        UpdateReplayUi();
    }

    void MainWindow::SeekReplay(uint64_t timestamp_ns)
    {
        if (!m_replay_loaded)
        {
            return;
        }
        timestamp_ns = (std::min)(
            timestamp_ns,
            m_session_replay.Info().duration_ns);

        ClearCatalogView();
        ResetDabOverviewHistory();
        ResetFicMeasurementSession();
        ResetMscMeasurementSession();
        ResetRfSyncDiagnostics();
        ResetRfChannelDiagnostics();
        ResetRfNullScope();
        m_rf_history_write = 0u;
        m_rf_history_count = 0u;
        m_latest_mer_valid = false;
        UpdateRfHistoryVisuals();
        ResetRfHistorySession();

        std::vector<dab::measurement::MeasurementSessionReplayRecord> state;
        std::string error;
        if (!m_session_replay.Seek(timestamp_ns, state, error))
        {
            m_replay_playing = false;
            AddTimelineEvent(L"Replay seek failed: " + to_hstring(error));
            return;
        }
        for (auto const& record : state)
        {
            if (!ApplyReplayRecord(record, false))
            {
                m_replay_playing = false;
                break;
            }
        }
        m_replay_position_ns = timestamp_ns;
        m_replay_last_tick = std::chrono::steady_clock::now();
    }

    bool MainWindow::ApplyReplayRecord(
        dab::measurement::MeasurementSessionReplayRecord const& record,
        bool include_event)
    {
        std::string error;
        switch (record.type)
        {
        case dab::measurement::MeasurementSessionRecordType::snapshot:
        {
            dab_receive_snapshot_t snapshot{};
            if (!dab::measurement::MeasurementSessionReplay::DecodeSnapshot(
                    record,
                    snapshot,
                    error))
            {
                break;
            }
            scoped_true applying{ m_replay_applying_record };
            AppendFicFrameTelemetry(snapshot);
            if (!m_fic_timeline_frozen)
            {
                UpdateFicTimelineVisuals();
                RefreshFicFailureList();
            }
            m_metrics_stale = false;
            ApplySnapshot(snapshot);
            return true;
        }
        case dab::measurement::MeasurementSessionRecordType::measurement:
        {
            dab::measurement::MeasurementFrame frame;
            if (!dab::measurement::MeasurementSessionReplay::DecodeMeasurement(
                    record,
                    frame,
                    error))
            {
                break;
            }
            scoped_true applying{ m_replay_applying_record };
            UpdatePlots(frame);
            return true;
        }
        case dab::measurement::MeasurementSessionRecordType::catalog:
        {
            dab_receive_catalog_t catalog{};
            if (!dab::measurement::MeasurementSessionReplay::DecodeCatalog(
                    record,
                    catalog,
                    error))
            {
                break;
            }
            scoped_true applying{ m_replay_applying_record };
            ApplyCatalog(catalog);
            return true;
        }
        case dab::measurement::MeasurementSessionRecordType::event:
        {
            if (!include_event)
            {
                return true;
            }
            uint32_t event_type = 0u;
            std::string message;
            if (!dab::measurement::MeasurementSessionReplay::DecodeEvent(
                    record,
                    event_type,
                    message,
                    error))
            {
                break;
            }
            AddTimelineEvent(
                L"Replay " +
                format_duration(record.timestamp_ns) +
                L" · " +
                to_hstring(message));
            return true;
        }
        case dab::measurement::MeasurementSessionRecordType::metadata:
            return true;
        default:
            error = "Unknown replay record type";
            break;
        }

        AddTimelineEvent(
            L"Replay data is incompatible: " + to_hstring(error));
        return false;
    }

    void MainWindow::UpdateReplayUi()
    {
        if (!m_replay_loaded)
        {
            return;
        }
        uint64_t const duration = m_session_replay.Info().duration_ns;
        m_replay_slider_internal = true;
        ReplayPositionSlider().Value(
            duration == 0u
                ? 0.0
                : 1000.0 *
                    static_cast<double>(m_replay_position_ns) /
                    static_cast<double>(duration));
        m_replay_slider_internal = false;
        ReplayPositionText().Text(
            format_duration(m_replay_position_ns) +
            L" / " +
            format_duration(duration));

        ReplayPlayPauseIcon().Glyph(
            m_replay_playing ? L"\uE769" : L"\uE768");
        ReplayPlayPauseText().Text(
            m_replay_playing ? L"Pause" :
            (m_replay_position_ns >= duration ? L"Replay" : L"Play"));

        hstring const replay_state =
            m_replay_playing
                ? hstring{ L"● REPLAY" }
                : (m_replay_position_ns >= duration
                    ? hstring{ L"REPLAY COMPLETE" }
                    : hstring{ L"REPLAY PAUSED" });
        DataModeText().Text(replay_state);
        DabModeBadgeText().Text(replay_state);
        RfLiveBadgeText().Text(replay_state);
        DataSeamText().Text(
            hstring{
                m_session_replay.Info().session_path.filename().wstring()
            } +
            L" · " +
            format_float(
                static_cast<float>(m_replay_speed),
                L"×",
                m_replay_speed < 1.0 ? 2 : 0));
        StatusRecordText().Text(L"replay");
        StatusRecordText().Foreground(
            Application::Current().Resources().Lookup(
                box_value(hstring{ L"MutedTextBrush" })).as<Brush>());

        StartButton().IsEnabled(false);
        StartButtonLabel().Text(L"Replay loaded");
        RecordButton().IsEnabled(false);
        DeviceComboBox().IsEnabled(false);
        ChannelComboBox().IsEnabled(false);
        FrequencyTextBox().IsEnabled(false);
        GainNumberBox().IsEnabled(false);
        UpdateRecordingTimelineCursor();
    }

    void MainWindow::MarkerButton_Click(IInspectable const&, RoutedEventArgs const&)
    {
        AddTimelineEvent(L"Marker — operator annotation");
    }

    void MainWindow::Window_Closed(IInspectable const&, WindowEventArgs const&)
    {
        m_timer.Stop();
        m_pending_receiver_restart.reset();
        m_receiver.StopAndJoin();
        m_session_recorder.Stop();
    }

    void MainWindow::OnTimerTick(IInspectable const&, IInspectable const&)
    {
        ++m_tick;
        if (m_tick == 1u && RootLayout())
        {
            ApplyWorkspaceWidth(RootLayout().ActualWidth());
        }
        if ((m_tick % 4u) == 0u)
        {
            UpdateCpuUsage();
        }

        dab::measurement::ReceiverStatus receiver_status;
        uint64_t status_version = 0;
        if (m_receiver.TryReadStatus(m_receiver_status_version, receiver_status, status_version))
        {
            m_receiver_status_version = status_version;
            if (!m_replay_loaded && !m_replay_importing)
            {
                HandleReceiverStatus(receiver_status);
            }
        }
        m_receiver.JoinFinished();
        if (m_replay_importing)
        {
            UpdateRecordingUi();
            return;
        }
        if (m_replay_loaded)
        {
            AdvanceReplay();
            UpdateReceiverMenuState();
            return;
        }
        StartPendingServiceRestart();

        std::vector<dab_receive_snapshot_t> fic_snapshots;
        uint64_t fic_history_version = 0u;
        if (m_mailbox.ReadSince(m_fic_mailbox_history_version,
                                fic_snapshots,
                                fic_history_version))
        {
            m_fic_mailbox_history_version = fic_history_version;
            for (auto const& fic_snapshot : fic_snapshots)
            {
                AppendFicFrameTelemetry(fic_snapshot);
            }
            if (!m_fic_timeline_frozen)
            {
                UpdateFicTimelineVisuals();
                RefreshFicFailureList();
            }
        }

        dab_receive_snapshot_t snapshot{};
        uint64_t snapshot_version = 0;
        if (m_mailbox.TryReadLatest(m_mailbox_version, snapshot, snapshot_version))
        {
            m_mailbox_version = snapshot_version;
            ApplySnapshot(snapshot);
        }

        dab::measurement::MeasurementFrame measurement_frame;
        uint64_t measurement_version = 0;
        if (m_measurement_mailbox.TryReadLatest(m_measurement_mailbox_version,
                                                measurement_frame,
                                                measurement_version))
        {
            m_measurement_mailbox_version = measurement_version;
            UpdatePlots(measurement_frame);
        }

        dab_receive_catalog_t catalog{};
        uint64_t catalog_version = 0;
        if (m_catalog_mailbox.TryReadLatest(m_catalog_mailbox_version,
                                            catalog,
                                            catalog_version))
        {
            m_catalog_mailbox_version = catalog_version;
            ApplyCatalog(catalog);
        }
        UpdateReceiverMenuState();
    }

    std::optional<dab::measurement::ReceiverSettings> MainWindow::ReadReceiverSettings()
    {
        std::wstring frequency_text{ FrequencyTextBox().Text().c_str() };
        wchar_t* frequency_end = nullptr;
        double const frequency_mhz = std::wcstod(frequency_text.c_str(), &frequency_end);
        if (frequency_end == frequency_text.c_str() || !std::isfinite(frequency_mhz) || frequency_mhz <= 0.0)
        {
            AddTimelineEvent(L"Invalid frequency — enter a value in MHz");
            return std::nullopt;
        }

        double gain_value = GainNumberBox().Value();
        if (!std::isfinite(gain_value))
        {
            gain_value = 40.0;
        }

        dab::measurement::ReceiverSettings settings;
        settings.driver = DeviceComboBox().SelectedIndex() == 1 ? "rtlsdr" : "sdrplay";
        settings.frequency_hz = static_cast<uint64_t>(std::llround(frequency_mhz * 1000000.0));
        settings.service_id = m_selected_service_id;
        settings.gain_db = static_cast<int>(std::clamp(std::lround(gain_value), 0l, 100l));
        settings.agc_enabled = m_receiver_agc_enabled;
        settings.ppm_correction = m_receiver_ppm_correction;
        settings.bias_tee_enabled = m_receiver_bias_tee_enabled;
        settings.use_real_device = true;

        StatusDeviceText().Text(settings.driver == "rtlsdr" ? L"RTL-SDR" : L"SDRplay RSP1A");
        auto selected_channel = ChannelComboBox().SelectedItem().try_as<ComboBoxItem>();
        if (selected_channel)
        {
            StatusChannelText().Text(unbox_value<hstring>(selected_channel.Content()));
        }
        return settings;
    }

    bool MainWindow::StartReceiver(dab::measurement::ReceiverSettings const& settings,
                                   bool clear_catalog,
                                   hstring const& timeline_message)
    {
        if (!m_receiver.Start(settings))
        {
            AddTimelineEvent(L"Receiver worker is already active");
            return false;
        }

        if (clear_catalog)
        {
            ClearCatalogView();
            ResetDabOverviewHistory();
            ResetFicMeasurementSession();
            ResetMscMeasurementSession();
            m_fic_mailbox_history_version = m_mailbox_version;
            m_rf_history_write = 0u;
            m_rf_history_count = 0u;
            m_latest_mer_valid = false;
            m_rf_was_locked = false;
            m_rf_lock_started_ns = 0u;
            UpdateRfHistoryVisuals();
            ResetRfHistorySession();
        }
        ResetRfSyncDiagnostics();
        ResetRfChannelDiagnostics();
        ResetRfNullScope();
        m_active_service_id = settings.service_id;
        SetReceiverControlsEnabled(false);
        StartButton().IsEnabled(true);
        StartButtonLabel().Text(L"Stop");
        StatusSyncText().Text(L"opening");
        DataModeText().Text(L"LIVE METRICS");
        DataSeamText().Text(L"awaiting receiver snapshots");
        AddTimelineEvent(timeline_message);
        return true;
    }

    void MainWindow::SelectAudioService(uint32_t service_id)
    {
        dab_receive_service_info_t const* selected_service = nullptr;
        for (uint32_t index = 0u; m_have_catalog && index < m_latest_catalog.service_count; ++index)
        {
            if (m_latest_catalog.services[index].service_id == service_id)
            {
                selected_service = &m_latest_catalog.services[index];
                break;
            }
        }
        if (selected_service == nullptr)
        {
            return;
        }

        bool has_audio = false;
        for (uint32_t index = 0u; index < selected_service->component_count; ++index)
        {
            if (selected_service->components[index].kind == DAB_RECEIVE_COMPONENT_STREAM_AUDIO)
            {
                has_audio = true;
                break;
            }
        }

        hstring service_name = fic_label_text(selected_service->label);
        if (service_name.empty())
        {
            wchar_t fallback[48]{};
            swprintf_s(fallback, L"SID 0x%08X", service_id);
            service_name = fallback;
        }
        if (!has_audio)
        {
            AddTimelineEvent(L"Cannot play " + service_name + L" — no stream-audio component");
            return;
        }

        m_selected_service_id = service_id;
        UpdateDabContext();
        if (m_replay_loaded)
        {
            AddTimelineEvent(
                L"Selected " + service_name +
                L" in replay · decoded audio payload was not recorded");
            DataSeamText().Text(L"replay selection " + service_name);
            return;
        }
        if (!m_receiver.IsActive())
        {
            AddTimelineEvent(L"Selected " + service_name + L" — press Start to play");
            DataSeamText().Text(L"selected " + service_name);
            return;
        }
        if (!m_pending_receiver_restart && m_active_service_id == service_id)
        {
            return;
        }

        auto settings = ReadReceiverSettings();
        if (!settings)
        {
            return;
        }
        settings->service_id = service_id;
        m_pending_receiver_restart = *settings;
        m_receiver.RequestStop();
        StartButton().IsEnabled(false);
        StartButtonLabel().Text(L"Switching...");
        DataSeamText().Text(L"switching to " + service_name);
        AddTimelineEvent(L"Switching audio service to " + service_name);
    }

    void MainWindow::StartPendingServiceRestart()
    {
        if (!m_pending_receiver_restart || m_receiver.IsActive())
        {
            return;
        }

        auto const settings = *m_pending_receiver_restart;
        m_pending_receiver_restart.reset();
        wchar_t message[80]{};
        swprintf_s(message, L"Starting selected service SID 0x%08X", settings.service_id);
        StartReceiver(settings, false, message);
    }

    void MainWindow::HandleReceiverStatus(dab::measurement::ReceiverStatus const& status)
    {
        AddTimelineEvent(status_text(status.message));
        DataSeamText().Text(status_text(status.message));

        using dab::measurement::ReceiverState;
        switch (status.state)
        {
        case ReceiverState::starting:
            m_metrics_stale = false;
            DabModeBadgeText().Text(L"STARTING");
            DabSyncBadgeText().Text(L"ACQUIRING");
            DabOverviewStatusValueText().Text(L"STARTING");
            DabOverviewStatusDetailText().Text(status_text(status.message));
            if (m_rf_null_controls_ready)
            {
                RfNullLiveText().Text(L"STARTING");
            }
            RfLiveBadgeText().Text(L"STARTING");
            SetReceiverControlsEnabled(false);
            StartButton().IsEnabled(true);
            StartButtonLabel().Text(L"Stop");
            StatusSyncText().Text(L"opening");
            StatusSyncIndicator().Fill(Application::Current().Resources().Lookup(
                box_value(hstring{ L"WarningBrush" })).as<Brush>());
            break;
        case ReceiverState::running:
            m_metrics_stale = false;
            DabModeBadgeText().Text(L"● LIVE");
            RfLiveBadgeText().Text(L"● LIVE");
            SetReceiverControlsEnabled(false);
            StartButton().IsEnabled(true);
            StartButtonLabel().Text(L"Stop");
            if (StatusSyncText().Text() == L"opening")
            {
                StatusSyncText().Text(L"acquiring");
            }
            break;
        case ReceiverState::recovering:
            m_metrics_stale = true;
            DabModeBadgeText().Text(L"STALE");
            DabSyncBadgeText().Text(L"RECOVERING");
            {
                Brush const warning = Application::Current().Resources().Lookup(
                    box_value(hstring{ L"WarningBrush" })).as<Brush>();
                DabPipelineRfIndicator().Fill(warning);
                DabPipelineFicIndicator().Fill(warning);
                DabPipelineMscIndicator().Fill(warning);
                DabPipelineDabIndicator().Fill(warning);
                DabPipelineAudioIndicator().Fill(warning);
                DabPipelineRfStateText().Text(L"STALE");
                DabPipelineFicStateText().Text(L"STALE");
                DabPipelineMscStateText().Text(L"STALE");
                DabPipelineDabStateText().Text(L"STALE");
                DabPipelineAudioStateText().Text(L"RESTARTING");
            }
            DabOverviewStatusValueText().Text(L"STALE");
            DabOverviewStatusDetailText().Text(status_text(status.message));
            if (m_rf_null_controls_ready)
            {
                RfNullLiveText().Text(L"STALE");
                RfNullControlStatusText().Text(L"Receiver recovering; last trace is stale");
            }
            RfLiveBadgeText().Text(L"STALE - RECOVERING");
            RfStatusValueText().Text(L"STALE");
            RfStatusDetailText().Text(status_text(status.message));
            RfSyncStateText().Text(L"RECOVERING");
            RfSyncTrackingText().Text(L"Automatic receiver recovery");
            RfSyncStateIndicator().Fill(Application::Current().Resources().Lookup(
                box_value(hstring{ L"WarningBrush" })).as<Brush>());
            RfLockIndicator().Fill(Application::Current().Resources().Lookup(
                box_value(hstring{ L"WarningBrush" })).as<Brush>());
            RfWarningsText().Text(
                L"Displayed RF values are stale. Receiver progress stopped; automatic recovery is active.");
            m_rf_was_locked = false;
            m_rf_lock_started_ns = 0u;
            SetReceiverControlsEnabled(false);
            StartButton().IsEnabled(true);
            StartButtonLabel().Text(L"Stop");
            StatusSyncText().Text(L"recovering");
            StatusSyncIndicator().Fill(Application::Current().Resources().Lookup(
                box_value(hstring{ L"WarningBrush" })).as<Brush>());
            break;
        case ReceiverState::stopping:
            DabModeBadgeText().Text(L"STOPPING");
            DabSyncBadgeText().Text(L"STOPPING");
            DabPipelineAudioStateText().Text(L"STOPPING");
            DabOverviewStatusValueText().Text(L"STOPPING");
            DabOverviewStatusDetailText().Text(status_text(status.message));
            if (m_rf_null_controls_ready)
            {
                RfNullLiveText().Text(L"STOPPING");
            }
            RfLiveBadgeText().Text(L"STOPPING");
            SetReceiverControlsEnabled(false);
            StartButton().IsEnabled(false);
            StartButtonLabel().Text(L"Stopping...");
            StatusSyncText().Text(L"stopping");
            StatusSyncIndicator().Fill(Application::Current().Resources().Lookup(
                box_value(hstring{ L"MutedTextBrush" })).as<Brush>());
            break;
        case ReceiverState::stopped:
            m_metrics_stale = false;
            DabModeBadgeText().Text(L"IDLE");
            DabSyncBadgeText().Text(L"NOT SYNCED");
            {
                Brush const inactive = Application::Current().Resources().Lookup(
                    box_value(hstring{ L"MutedTextBrush" })).as<Brush>();
                DabPipelineRfIndicator().Fill(inactive);
                DabPipelineFicIndicator().Fill(inactive);
                DabPipelineMscIndicator().Fill(inactive);
                DabPipelineDabIndicator().Fill(inactive);
                DabPipelineAudioIndicator().Fill(inactive);
                DabPipelineRfStateText().Text(L"IDLE");
                DabPipelineFicStateText().Text(L"RETAINED");
                DabPipelineMscStateText().Text(L"RETAINED");
                DabPipelineDabStateText().Text(L"RETAINED");
                DabPipelineAudioStateText().Text(L"STOPPED");
            }
            DabOverviewStatusValueText().Text(L"IDLE");
            DabOverviewStatusDetailText().Text(L"Receiver stopped; last measurements retained");
            if (m_rf_null_controls_ready)
            {
                RfNullLiveText().Text(L"IDLE");
                RfNullControlStatusText().Text(L"Receiver stopped; last trace retained");
            }
            m_active_service_id = 0u;
            RfLiveBadgeText().Text(L"IDLE");
            RfStatusValueText().Text(L"IDLE");
            RfStatusDetailText().Text(L"Receiver stopped");
            RfSyncStateText().Text(L"IDLE");
            RfSyncTrackingText().Text(L"Idle");
            RfSyncStateIndicator().Fill(Application::Current().Resources().Lookup(
                box_value(hstring{ L"MutedTextBrush" })).as<Brush>());
            m_rf_was_locked = false;
            m_rf_lock_started_ns = 0u;
            SetReceiverControlsEnabled(true);
            StartButton().IsEnabled(true);
            StartButtonLabel().Text(L"Start");
            StatusSyncText().Text(L"idle");
            StatusSyncIndicator().Fill(Application::Current().Resources().Lookup(
                box_value(hstring{ L"MutedTextBrush" })).as<Brush>());
            if (!m_pending_receiver_restart.has_value())
            {
                StopMeasurementRecording(
                    L"Measurement recording stopped with the receiver");
            }
            break;
        case ReceiverState::failed:
            m_metrics_stale = false;
            DabModeBadgeText().Text(L"ERROR");
            DabSyncBadgeText().Text(L"NOT SYNCED");
            {
                Brush const warning = Application::Current().Resources().Lookup(
                    box_value(hstring{ L"WarningBrush" })).as<Brush>();
                DabPipelineRfIndicator().Fill(warning);
                DabPipelineFicIndicator().Fill(warning);
                DabPipelineMscIndicator().Fill(warning);
                DabPipelineDabIndicator().Fill(warning);
                DabPipelineAudioIndicator().Fill(warning);
                DabPipelineRfStateText().Text(L"ERROR");
                DabPipelineFicStateText().Text(L"STALE");
                DabPipelineMscStateText().Text(L"STALE");
                DabPipelineDabStateText().Text(L"STALE");
                DabPipelineAudioStateText().Text(L"STOPPED");
            }
            DabOverviewStatusValueText().Text(L"ERROR");
            DabOverviewStatusDetailText().Text(status_text(status.message));
            if (m_rf_null_controls_ready)
            {
                RfNullLiveText().Text(L"ERROR");
                RfNullControlStatusText().Text(L"Receiver error; last trace retained");
            }
            m_active_service_id = 0u;
            RfLiveBadgeText().Text(L"ERROR");
            RfStatusValueText().Text(L"ERROR");
            RfStatusDetailText().Text(status_text(status.message));
            RfSyncStateText().Text(L"ERROR");
            RfSyncTrackingText().Text(L"Receiver error");
            RfSyncStateIndicator().Fill(Application::Current().Resources().Lookup(
                box_value(hstring{ L"WarningBrush" })).as<Brush>());
            SetReceiverControlsEnabled(true);
            StartButton().IsEnabled(true);
            StartButtonLabel().Text(L"Start");
            StatusSyncText().Text(L"error");
            StatusSyncIndicator().Fill(Application::Current().Resources().Lookup(
                box_value(hstring{ L"WarningBrush" })).as<Brush>());
            StopMeasurementRecording(
                L"Measurement recording stopped after receiver failure");
            break;
        }

        if (status.state == ReceiverState::recovering)
        {
            DataModeText().Text(L"STALE METRICS");
        }
        else if (status.is_error)
        {
            DataModeText().Text(L"RECEIVER ERROR");
        }
        else if (status.state == ReceiverState::stopped)
        {
            DataModeText().Text(L"LIVE METRICS");
        }
    }

    void MainWindow::SetReceiverControlsEnabled(bool enabled)
    {
        DeviceComboBox().IsEnabled(enabled);
        ChannelComboBox().IsEnabled(enabled);
        FrequencyTextBox().IsEnabled(enabled);
        GainNumberBox().IsEnabled(enabled && !m_receiver_agc_enabled);
        UpdateReceiverMenuState();
    }

    void MainWindow::UpdateReceiverMenuState()
    {
        if (!m_receiver_menu_ready)
        {
            return;
        }

        bool const active = m_receiver.IsActive();
        bool const replay_unavailable =
            m_replay_loaded || m_replay_importing;
        ReceiverStartStopMenuItem().Text(
            m_replay_loaded
                ? L"Live receiver unavailable during replay"
                : (active ? L"Stop receiver" : L"Start receiver"));
        ReceiverStartStopMenuItem().IsEnabled(
            !replay_unavailable && StartButton().IsEnabled());
        ReceiverDeviceSubmenu().IsEnabled(!active && !replay_unavailable);
        ReceiverChannelSubmenu().IsEnabled(!active && !replay_unavailable);
        ReceiverEnterFrequencyMenuItem().IsEnabled(
            !active && !replay_unavailable);
        ReceiverGainSubmenu().IsEnabled(
            !active && !replay_unavailable && !m_receiver_agc_enabled);
        ReceiverFrontEndSubmenu().IsEnabled(
            !active && !replay_unavailable);
        ReceiverPpmSubmenu().IsEnabled(
            !active && !replay_unavailable);
        UpdateRecordingUi();
        if (m_replay_loaded && !m_replay_importing)
        {
            UpdateReplayUi();
        }

        int32_t const device_index = DeviceComboBox().SelectedIndex();
        ReceiverSdrplayMenuItem().IsChecked(device_index != 1);
        ReceiverRtlSdrMenuItem().IsChecked(device_index == 1);
        ReceiverAgcMenuItem().IsChecked(m_receiver_agc_enabled);
        ReceiverBiasTeeMenuItem().IsChecked(m_receiver_bias_tee_enabled);

        auto const selected_channel =
            ChannelComboBox().SelectedItem().try_as<ComboBoxItem>();
        if (selected_channel)
        {
            ReceiverChannelStatusMenuItem().Text(
                L"Current: " +
                unbox_value<hstring>(selected_channel.Content()) +
                L" \u00b7 " +
                unbox_value<hstring>(selected_channel.Tag()) +
                L" MHz");
        }

        double gain = GainNumberBox().Value();
        if (!std::isfinite(gain))
        {
            gain = 40.0;
        }
        wchar_t gain_text[48]{};
        swprintf_s(gain_text,
                   m_receiver_agc_enabled ? L"Current: AGC (manual %ld dB)" : L"Current: %ld dB",
                   std::lround(gain));
        ReceiverGainStatusMenuItem().Text(gain_text);

        wchar_t ppm_text[40]{};
        swprintf_s(ppm_text, L"Current: %+d ppm", m_receiver_ppm_correction);
        ReceiverPpmStatusMenuItem().Text(ppm_text);
    }

    void MainWindow::UpdateViewMenuState()
    {
        if (!m_receiver_menu_ready)
        {
            return;
        }

        ViewReceiverWorkspaceMenuItem().IsChecked(
            m_active_workspace == WorkspaceKind::receiver);
        ViewRfWorkspaceMenuItem().IsChecked(
            m_active_workspace == WorkspaceKind::rf_measurements);
        ViewDabWorkspaceMenuItem().IsChecked(
            m_active_workspace == WorkspaceKind::dab_measurements);
        ViewRecordingWorkspaceMenuItem().IsChecked(
            m_active_workspace == WorkspaceKind::recording_replay);
        ViewAnalysisPanelMaximizedMenuItem().IsChecked(m_cir_panel_maximized);
        ViewToolbarMenuItem().IsChecked(m_toolbar_visible);
        ViewStatusBarMenuItem().IsChecked(m_status_bar_visible);
        ViewFullScreenMenuItem().IsChecked(m_full_screen);
    }

    void MainWindow::ShowWorkspace(WorkspaceKind workspace)
    {
        if (workspace != WorkspaceKind::receiver && m_cir_panel_maximized)
        {
            SetCirPanelMaximized(false);
        }
        bool const receiver_active = workspace == WorkspaceKind::receiver;
        bool const rf_active = workspace == WorkspaceKind::rf_measurements;
        bool const dab_active = workspace == WorkspaceKind::dab_measurements;
        bool const recording_active =
            workspace == WorkspaceKind::recording_replay;
        bool const changed = m_active_workspace != workspace;
        m_active_workspace = workspace;
        m_rf_workspace_active = rf_active;
        ReceiverWorkspaceMain().Visibility(receiver_active ? Visibility::Visible : Visibility::Collapsed);
        ReceiverTimelinePanel().Visibility(receiver_active ? Visibility::Visible : Visibility::Collapsed);
        RfWorkspace().Visibility(rf_active ? Visibility::Visible : Visibility::Collapsed);
        DabWorkspace().Visibility(dab_active ? Visibility::Visible : Visibility::Collapsed);
        RecordingReplayWorkspace().Visibility(
            recording_active ? Visibility::Visible : Visibility::Collapsed);
        ReceiverNavIndicator().Visibility(receiver_active ? Visibility::Visible : Visibility::Collapsed);
        RfNavIndicator().Visibility(rf_active ? Visibility::Visible : Visibility::Collapsed);
        DabNavIndicator().Visibility(dab_active ? Visibility::Visible : Visibility::Collapsed);
        RecordingNavIndicator().Visibility(
            recording_active ? Visibility::Visible : Visibility::Collapsed);
        ReceiverNavigationButton().Opacity(receiver_active ? 1.0 : 0.70);
        RfNavigationButton().Opacity(rf_active ? 1.0 : 0.70);
        DabNavigationButton().Opacity(dab_active ? 1.0 : 0.70);
        RecordingNavigationButton().Opacity(
            recording_active ? 1.0 : 0.70);
        RfToolbarButton().Opacity(rf_active ? 1.0 : 0.82);
        DabToolbarButton().Opacity(dab_active ? 1.0 : 0.82);
        ApplyWorkspaceWidth(RootLayout().ActualWidth());
        if (rf_active)
        {
            UpdateRfContext();
        }
        else if (dab_active)
        {
            UpdateDabContext();
        }
        else if (recording_active)
        {
            UpdateRecordingWorkspace();
        }
        if (changed)
        {
            hstring message{ L"Opened Receiver workspace" };
            if (rf_active)
            {
                message = L"Opened RF Measurements workspace";
            }
            else if (dab_active)
            {
                message = L"Opened DAB Measurements workspace";
            }
            else if (recording_active)
            {
                message = L"Opened Recording & Replay workspace";
            }
            AddTimelineEvent(message);
        }
        UpdateViewMenuState();
    }

    void MainWindow::UpdateRfContext()
    {
        // Channel selection fires while InitializeComponent is still creating
        // controls that appear later in the XAML document.
        if (!RfContextText())
        {
            return;
        }

        hstring device{ L"Receiver" };
        hstring channel{ L"—" };
        hstring frequency{ L"— MHz" };
        if (auto const selected_device = DeviceComboBox().SelectedItem().try_as<ComboBoxItem>())
        {
            device = unbox_value<hstring>(selected_device.Content());
        }
        if (auto const selected_channel = ChannelComboBox().SelectedItem().try_as<ComboBoxItem>())
        {
            channel = unbox_value<hstring>(selected_channel.Content());
        }
        if (FrequencyTextBox())
        {
            frequency = FrequencyTextBox().Text();
        }
        hstring serial{ L"not reported" };
        if (m_have_latest_rf_snapshot &&
            m_latest_rf_snapshot.device_serial[0] != '\0')
        {
            serial = to_hstring(m_latest_rf_snapshot.device_serial);
        }
        RfContextText().Text(device + L" · Serial " + serial +
                            L" · Channel " + channel + L" · " + frequency);

        if (m_have_latest_rf_snapshot &&
            m_latest_rf_snapshot.applied_system_gain_valid != 0)
        {
            RfConfigGainText().Text(
                format_float(m_latest_rf_snapshot.applied_system_gain_db,
                             L" dB",
                             1));
        }
        else
        {
            double gain = GainNumberBox().Value();
            if (!std::isfinite(gain))
            {
                gain = 40.0;
            }
            wchar_t gain_text[32]{};
            swprintf_s(gain_text, L"%.0f dB requested", gain);
            RfConfigGainText().Text(gain_text);
        }
        RfConfigAgcText().Text(m_receiver_agc_enabled
            ? L"Enabled"
            : L"Disabled");
        RfConfigAntennaText().Text(
            m_have_latest_rf_snapshot &&
            m_latest_rf_snapshot.device_antenna != 0u
                ? hstring{ L"B" }
                : hstring{ L"A" });
    }

    void MainWindow::UpdateDabContext()
    {
        // Channel selection can run while controls later in the XAML document
        // are still being created.
        if (!DabContextText())
        {
            return;
        }

        hstring channel{ L"—" };
        hstring frequency{ L"— MHz" };
        if (auto const selected_channel = ChannelComboBox().SelectedItem().try_as<ComboBoxItem>())
        {
            channel = unbox_value<hstring>(selected_channel.Content());
        }
        if (FrequencyTextBox())
        {
            frequency = FrequencyTextBox().Text();
        }

        hstring ensemble{ L"Awaiting FIC" };
        if (m_have_catalog)
        {
            ensemble = fic_label_text(m_latest_catalog.ensemble_label);
            if (ensemble.empty())
            {
                wchar_t fallback[48]{};
                swprintf_s(fallback, L"Ensemble 0x%04X", m_latest_catalog.ensemble_id);
                ensemble = fallback;
            }
        }

        dab_receive_service_info_t const* service = nullptr;
        if (m_selected_service_id != 0u)
        {
            for (uint32_t index = 0u; index < m_latest_catalog.service_count; ++index)
            {
                if (m_latest_catalog.services[index].service_id == m_selected_service_id)
                {
                    service = &m_latest_catalog.services[index];
                    break;
                }
            }
        }
        if (service == nullptr)
        {
            int32_t const selected_index = ServicesListView().SelectedIndex();
            if (m_have_catalog && selected_index >= 0 &&
                static_cast<uint32_t>(selected_index) < m_latest_catalog.service_count)
            {
                service = &m_latest_catalog.services[static_cast<uint32_t>(selected_index)];
            }
        }

        hstring service_name{ L"Not selected" };
        hstring service_id{ L"—" };
        if (service != nullptr)
        {
            service_name = fic_label_text(service->label);
            if (service_name.empty())
            {
                service_name = L"Unnamed service";
            }
            wchar_t id_text[32]{};
            swprintf_s(id_text, L"0x%08X", service->service_id);
            service_id = id_text;
        }

        dab_receive_component_info_t const* component = nullptr;
        if (service != nullptr)
        {
            for (uint32_t index = 0u; index < service->component_count; ++index)
            {
                auto const& candidate = service->components[index];
                if (candidate.kind == DAB_RECEIVE_COMPONENT_STREAM_AUDIO &&
                    candidate.primary != 0u)
                {
                    component = &candidate;
                    break;
                }
            }
            if (component == nullptr)
            {
                for (uint32_t index = 0u; index < service->component_count; ++index)
                {
                    if (service->components[index].kind == DAB_RECEIVE_COMPONENT_STREAM_AUDIO)
                    {
                        component = &service->components[index];
                        break;
                    }
                }
            }
            if (component == nullptr && service->component_count != 0u)
            {
                component = &service->components[0];
            }
        }

        hstring component_name{ L"Awaiting service component" };
        hstring component_detail{ L"Awaiting service component" };
        hstring subchannel{ L"—" };
        hstring protection{ L"—" };
        if (component != nullptr)
        {
            component_name = component_kind_text(component->kind);
            if (component->primary != 0u)
            {
                component_name = component_name + L" · primary";
            }

            wchar_t detail[160]{};
            if (component->has_subchannel != 0u)
            {
                swprintf_s(detail,
                           L"%s · SubCh %u · SC type %u",
                           component_name.c_str(),
                           component->subchannel_id,
                           component->service_component_type);
                component_detail = detail;
                subchannel = to_hstring(component->subchannel_id);
                if (component->subchannel_valid != 0u)
                {
                    wchar_t protection_text[96]{};
                    if (component->protection_kind == 0u)
                    {
                        swprintf_s(protection_text,
                                   L"UEP · table %u · index %u · %u CU",
                                   component->protection_table_switch,
                                   component->protection_index,
                                   component->subchannel_size_cu);
                    }
                    else
                    {
                        swprintf_s(protection_text,
                                   L"EEP %u-%s · %u CU",
                                   component->protection_level + 1u,
                                   component->protection_option == 0u ? L"A" : L"B",
                                   component->subchannel_size_cu);
                    }
                    protection = protection_text;
                }
                else
                {
                    protection = L"Allocation pending";
                }
            }
            else
            {
                wchar_t component_text[96]{};
                swprintf_s(component_text,
                           L"%s · component %u · SC type %u",
                           component_name.c_str(),
                           component->component_index,
                           component->service_component_type);
                component_detail = component_text;
            }
        }

        DabContextText().Text(channel + L" · " + frequency + L" · Ensemble: " +
                              ensemble + L" · Service: " + service_name);
        if (DabPlusHeaderContextText())
        {
            DabPlusHeaderContextText().Text(
                channel + L" · " + frequency + L" · Ensemble: " +
                ensemble + L" · Service: " + service_name +
                L" · Component: " + component_name);
        }
        DabComponentContextText().Text(L"Component: " + component_detail);
        DabOverviewEnsembleText().Text(ensemble);
        DabOverviewServiceText().Text(service_name);
        DabOverviewComponentText().Text(component_detail);
        DabOverviewServiceIdText().Text(service_id);
        DabMscServiceText().Text(service_name);
        DabMscComponentText().Text(component_name);
        DabMscSubchannelText().Text(subchannel);
        DabMscProtectionText().Text(protection);
        if (component != nullptr && component->subchannel_bitrate_kbps != 0u)
        {
            DabOverviewServiceValueText().Text(
                to_hstring(component->subchannel_bitrate_kbps) + L" kbit/s");
            DabOverviewServiceDetailText().Text(protection + L" · " + service_name);
        }
        else
        {
            DabOverviewServiceValueText().Text(L"—");
            DabOverviewServiceDetailText().Text(service == nullptr
                ? hstring{ L"No service selected" }
                : hstring{ L"Bitrate not resolved for this component" });
        }

        DabFicEnsembleText().Text(ensemble);
        if (m_have_catalog)
        {
            wchar_t ensemble_id[24]{};
            swprintf_s(ensemble_id, L"0x%04X", m_latest_catalog.ensemble_id);
            DabFicEnsembleIdText().Text(ensemble_id);
            DabFicServicesText().Text(to_hstring(m_latest_catalog.service_count));
            DabFicCatalogSequenceText().Text(to_hstring(m_latest_catalog.sequence));
        }
        else
        {
            DabFicEnsembleIdText().Text(L"—");
            DabFicServicesText().Text(L"0");
            DabFicCatalogSequenceText().Text(L"—");
        }
    }

    void MainWindow::ApplyFicSnapshot(dab_receive_snapshot_t const& snapshot)
    {
        bool const valid = snapshot.locked != 0 &&
                           snapshot.fic_valid_fibs_last_frame != 0u;
        Brush const status_brush = Application::Current().Resources().Lookup(
            box_value(hstring{ valid
                ? L"MeasurementAccentBrush"
                : (snapshot.locked != 0 ? L"WarningBrush" : L"MutedTextBrush") })).as<Brush>();
        FicHeaderStatusIndicator().Fill(status_brush);
        FicHeaderStatusText().Foreground(status_brush);
        FicHeaderStatusText().Text(valid ? L"VALID" :
            (snapshot.locked != 0 ? L"DEGRADED" : L"NOT LOCKED"));

        hstring ensemble{ L"Awaiting ensemble" };
        hstring ensemble_id{ L"—" };
        if (m_have_catalog)
        {
            ensemble = fic_label_text(m_latest_catalog.ensemble_label);
            if (ensemble.empty())
            {
                ensemble = L"Unnamed ensemble";
            }
            wchar_t id[24]{};
            swprintf_s(id, L"0x%04X", m_latest_catalog.ensemble_id);
            ensemble_id = id;
        }
        uint64_t const stable_seconds = m_tick >= m_dab_catalog_changed_tick
            ? (m_tick - m_dab_catalog_changed_tick) / 4u
            : 0u;
        FicHeaderContextText().Text(
            L"Ensemble " + ensemble_id + L" · " + ensemble +
            L" · Configuration stable for " +
            format_duration(stable_seconds * 1000000000ull));

        constexpr uint64_t fibs_per_frame = DAB_RECEIVE_FIC_FIB_COUNT;
        uint64_t const total_fibs =
            static_cast<uint64_t>(snapshot.fic_frames_seen) * fibs_per_frame;
        uint64_t const valid_fibs = snapshot.fic_total_valid_fibs;
        uint64_t const crc_errors = total_fibs >= valid_fibs
            ? total_fibs - valid_fibs
            : 0u;
        bool const have_fibs = total_fibs != 0u;
        float const valid_percent = have_fibs
            ? 100.0f * static_cast<float>(
                static_cast<double>(valid_fibs) / static_cast<double>(total_fibs))
            : 0.0f;
        FicCrcValidValueText().Text(have_fibs
            ? format_float(valid_percent, L"%", 2)
            : hstring{ L"—" });
        FicCrcValidDetailText().Text(have_fibs
            ? to_hstring(valid_fibs) + L" / " + to_hstring(total_fibs)
            : hstring{ L"Awaiting decoded FIBs" });
        if (crc_errors > m_fic_last_total_errors)
        {
            m_fic_last_failure_tick = m_tick;
        }
        m_fic_last_total_errors = crc_errors;
        FicCrcErrorsValueText().Text(to_hstring(crc_errors));
        if (crc_errors == 0u)
        {
            FicCrcErrorsDetailText().Text(L"No errors since session start");
        }
        else
        {
            uint64_t const elapsed_seconds =
                m_tick >= m_fic_last_failure_tick
                    ? (m_tick - m_fic_last_failure_tick) / 4u
                    : 0u;
            FicCrcErrorsDetailText().Text(
                L"Last " + format_duration(elapsed_seconds * 1000000000ull) + L" ago");
        }

        FicPreBerValueText().Text(snapshot.fic_pre_viterbi_ber_valid != 0
            ? format_scientific(snapshot.fic_pre_viterbi_ber)
            : hstring{ L"—" });
        FicPreBerDetailText().Text(snapshot.fic_pre_viterbi_ber_valid == 0
            ? hstring{ L"Awaiting compared channel bits" }
            : (snapshot.fic_pre_viterbi_ber < 1.0e-3f
                ? hstring{ L"Stable" }
                : hstring{ L"Degraded" }));
        FicFigErrorsValueText().Text(
            to_hstring(snapshot.fic_total_fig_parse_errors));
        FicLockValueText().Text(valid ? L"Locked" :
            (snapshot.locked != 0 ? L"Partial" : L"Unlocked"));
        FicLockDetailText().Text(snapshot.sync_lock_duration_ns != 0u
            ? format_duration(snapshot.sync_lock_duration_ns)
            : hstring{ L"Awaiting frame lock" });
        FicViterbiValueText().Text(snapshot.fic_frames_seen != 0u
            ? format_float(snapshot.fic_viterbi_quality_percent, L"%", 1)
            : hstring{ L"—" });
        FicViterbiDetailText().Text(snapshot.fic_frames_seen == 0u
            ? hstring{ L"Awaiting channel softbits" }
            : (snapshot.fic_viterbi_quality_percent >= 70.0f
                ? hstring{ L"Good input confidence" }
                : hstring{ L"Low input confidence" }));

        uint64_t const rate_timestamp = snapshot.timestamp_ns;
        if (m_fic_previous_rate_timestamp_ns != 0u &&
            rate_timestamp > m_fic_previous_rate_timestamp_ns &&
            snapshot.fic_total_figs >= m_fic_previous_rate_count)
        {
            double const seconds =
                static_cast<double>(rate_timestamp -
                                    m_fic_previous_rate_timestamp_ns) /
                1000000000.0;
            float const instantaneous = static_cast<float>(
                static_cast<double>(snapshot.fic_total_figs -
                                    m_fic_previous_rate_count) /
                seconds);
            m_fic_rate = m_fic_rate == 0.0f
                ? instantaneous
                : 0.75f * m_fic_rate + 0.25f * instantaneous;
        }

        uint64_t change_total = 0u;
        if (!m_fic_browser_frozen)
        {
            double const stat_seconds =
                m_fic_previous_rate_timestamp_ns != 0u &&
                rate_timestamp > m_fic_previous_rate_timestamp_ns
                    ? static_cast<double>(rate_timestamp -
                                          m_fic_previous_rate_timestamp_ns) /
                        1000000000.0
                    : 0.0;
            m_fic_fig_stat_count = (std::min)(
                snapshot.fic_fig_stat_count,
                static_cast<uint32_t>(DAB_RECEIVE_FIC_FIG_STAT_CAPACITY));
            for (uint32_t index = 0u; index < m_fic_fig_stat_count; ++index)
            {
                auto const& source = snapshot.fic_fig_stats[index];
                size_t const key =
                    (static_cast<size_t>(source.type & 0x07u) << 5u) |
                    static_cast<size_t>(source.extension & 0x1Fu);
                if (stat_seconds > 0.0 &&
                    source.total_count >= m_fic_fig_previous_counts[key])
                {
                    float const instantaneous = static_cast<float>(
                        static_cast<double>(
                            source.total_count - m_fic_fig_previous_counts[key]) /
                        stat_seconds);
                    m_fic_fig_rates[key] = m_fic_fig_rates[key] == 0.0f
                        ? instantaneous
                        : 0.75f * m_fic_fig_rates[key] + 0.25f * instantaneous;
                }
                m_fic_fig_previous_counts[key] = source.total_count;
                m_fic_fig_stats[index] = source;
                if (source.supported != 0u)
                {
                    change_total += source.change_count;
                }
            }
            m_fic_latest_frame_index = snapshot.fic_frames_seen;
            m_fic_latest_timestamp_ns = snapshot.timestamp_ns;
        }
        m_fic_previous_rate_count = snapshot.fic_total_figs;
        m_fic_previous_rate_timestamp_ns = rate_timestamp;

        FicFigRateValueText().Text(snapshot.fic_total_figs != 0u
            ? format_float(m_fic_rate, L" FIG/s", 1)
            : hstring{ L"—" });
        FicFigRateDetailText().Text(
            to_hstring(m_fic_fig_stat_count) + L" type/extension pairs active");
        if (snapshot.fic_change_flag == 0u)
        {
            FicReconfigValueText().Text(L"NONE PENDING");
            FicReconfigDetailText().Text(m_have_catalog
                ? hstring{ L"Database current" }
                : hstring{ L"Awaiting database" });
        }
        else
        {
            FicReconfigValueText().Text(
                L"CHANGE FLAG " + to_hstring(snapshot.fic_change_flag));
            FicReconfigDetailText().Text(snapshot.fic_alarm_flag != 0u
                ? hstring{ L"Alarm flag set" }
                : hstring{ L"Configuration transition signalled" });
        }

        bool const configuration_changed =
            !m_fic_browser_frozen &&
            m_fic_change_baseline_valid &&
            change_total > m_fic_last_seen_change_total;
        if (configuration_changed)
        {
            m_fic_last_seen_change_total = change_total;
            if (FicCaptureIqOnChangeCheckBox().IsChecked() &&
                FicCaptureIqOnChangeCheckBox().IsChecked().Value())
            {
                m_fic_capture_in_progress = true;
                RfNullCaptureIq_Click(IInspectable{ nullptr },
                                      RoutedEventArgs{ nullptr });
            }
            if (FicFreezeOnChangeCheckBox().IsChecked() &&
                FicFreezeOnChangeCheckBox().IsChecked().Value())
            {
                m_fic_browser_frozen = true;
                FicFigBrowserStatusText().Text(L"FROZEN · FIG content changed");
                AddTimelineEvent(L"FIG browser frozen on signalling change");
            }
        }
        if (!m_fic_browser_frozen)
        {
            m_fic_last_seen_change_total = change_total;
            m_fic_change_baseline_valid = true;
            RefreshFicBrowser();
        }
    }

    void MainWindow::AppendFicFrameTelemetry(
        dab_receive_snapshot_t const& snapshot)
    {
        if (snapshot.fic_frames_seen == 0u ||
            snapshot.fic_frame_start_sample == 0u ||
            (!m_fic_fib_history.empty() &&
             m_fic_fib_history.back().frame_start_sample ==
                snapshot.fic_frame_start_sample))
        {
            return;
        }

        uint64_t const wall_time = current_wall_time_100ns();
        for (uint32_t fib = 0u; fib < DAB_RECEIVE_FIC_FIB_COUNT; ++fib)
        {
            uint32_t const block = fib / 3u;
            FicFibHistoryEntry entry{};
            entry.timestamp_ns = snapshot.timestamp_ns;
            entry.wall_time_100ns = wall_time;
            entry.frame_start_sample = snapshot.fic_frame_start_sample;
            entry.frame_index = snapshot.fic_frames_seen;
            entry.fib_index = static_cast<uint8_t>(fib);
            entry.crc_valid =
                (snapshot.fic_crc_valid_mask & (1u << fib)) != 0u;
            entry.pre_viterbi_ber =
                snapshot.fic_block_pre_viterbi_ber[block];
            entry.viterbi_quality_percent =
                snapshot.fic_block_viterbi_quality_percent[block];
            std::copy(std::begin(snapshot.fic_decoded_fibs[fib]),
                      std::end(snapshot.fic_decoded_fibs[fib]),
                      entry.raw.begin());
            m_fic_fib_history.push_back(entry);
        }
        while (m_fic_fib_history.size() > fic_fib_history_capacity)
        {
            m_fic_fib_history.pop_front();
        }
    }

    void MainWindow::UpdateFicTimelineVisuals()
    {
        auto cells = FicCrcTimelineCells().Children();
        cells.Clear();
        PointCollection quality_points;
        PointCollection ber_points;
        if (m_fic_fib_history.empty())
        {
            FicViterbiTimelineTrace().Points(quality_points);
            FicBerTimelineTrace().Points(ber_points);
            return;
        }

        uint64_t period_seconds = 10u;
        auto const period_item =
            FicTimelinePeriodComboBox().SelectedItem().try_as<ComboBoxItem>();
        if (period_item && period_item.Tag())
        {
            period_seconds = std::wcstoull(
                unbox_value<hstring>(period_item.Tag()).c_str(),
                nullptr,
                10);
        }
        uint64_t const latest_ns = m_fic_fib_history.back().timestamp_ns;
        uint64_t const start_ns = latest_ns > period_seconds * 1000000000ull
            ? latest_ns - period_seconds * 1000000000ull
            : 0u;
        size_t first = 0u;
        while (first < m_fic_fib_history.size() &&
               m_fic_fib_history[first].timestamp_ns < start_ns)
        {
            ++first;
        }
        if (first >= m_fic_fib_history.size())
        {
            return;
        }

        size_t const count = m_fic_fib_history.size() - first;
        size_t const cell_first = count > 80u
            ? m_fic_fib_history.size() - 80u
            : first;
        Brush const valid_brush = Application::Current().Resources().Lookup(
            box_value(hstring{ L"MeasurementAccentBrush" })).as<Brush>();
        Brush const error_brush = Application::Current().Resources().Lookup(
            box_value(hstring{ L"WarningBrush" })).as<Brush>();
        for (size_t index = cell_first;
             index < m_fic_fib_history.size();
             ++index)
        {
            winrt::Microsoft::UI::Xaml::Shapes::Rectangle cell;
            cell.Width(8.0);
            cell.Height(14.0);
            cell.RadiusX(1.0);
            cell.RadiusY(1.0);
            cell.Fill(m_fic_fib_history[index].crc_valid
                ? valid_brush
                : error_brush);
            cells.Append(cell);
        }

        constexpr float left = 105.0f;
        constexpr float width = 790.0f;
        for (size_t point = 0u; point < count; ++point)
        {
            auto const& entry = m_fic_fib_history[first + point];
            float const x = count > 1u
                ? left + width * static_cast<float>(point) /
                    static_cast<float>(count - 1u)
                : left + width;
            float const quality = std::clamp(
                entry.viterbi_quality_percent / 100.0f,
                0.0f,
                1.0f);
            quality_points.Append(Point{ x, 58.0f - quality * 28.0f });
            float const log_ber = std::log10(
                std::clamp(entry.pre_viterbi_ber, 1.0e-7f, 1.0e-1f));
            float const good = std::clamp((-1.0f - log_ber) / 6.0f,
                                          0.0f,
                                          1.0f);
            ber_points.Append(Point{ x, 118.0f - good * 28.0f });
        }
        FicViterbiTimelineTrace().Points(quality_points);
        FicBerTimelineTrace().Points(ber_points);
    }

    void MainWindow::RefreshFicFailureList()
    {
        auto items = FicFailureListView().Items();
        items.Clear();
        if (m_fic_fib_history.empty())
        {
            return;
        }
        uint64_t const latest_ns = m_fic_fib_history.back().timestamp_ns;
        uint64_t const start_ns = latest_ns > 10000000000ull
            ? latest_ns - 10000000000ull
            : 0u;
        uint32_t added = 0u;
        for (size_t reverse = m_fic_fib_history.size();
             reverse > 0u && added < 40u;
             --reverse)
        {
            size_t const index = reverse - 1u;
            auto const& entry = m_fic_fib_history[index];
            if (entry.timestamp_ns < start_ns)
            {
                break;
            }
            if (entry.crc_valid)
            {
                continue;
            }

            StackPanel row;
            row.Orientation(Orientation::Horizontal);
            row.Tag(box_value(static_cast<uint64_t>(index)));
            auto append = [&](hstring const& text, double width)
            {
                TextBlock value;
                value.Text(text);
                value.Width(width);
                value.TextTrimming(TextTrimming::CharacterEllipsis);
                row.Children().Append(value);
            };
            append(history_clock_ms_text(entry.wall_time_100ns), 145.0);
            append(to_hstring(entry.frame_index), 150.0);
            append(to_hstring(entry.fib_index), 60.0);
            append(L"Error", 80.0);
            append(format_scientific(entry.pre_viterbi_ber), 115.0);
            TextBlock action;
            action.Text(L"Inspect →");
            action.Foreground(Application::Current().Resources().Lookup(
                box_value(hstring{ L"SpectrumBrush" })).as<Brush>());
            row.Children().Append(action);
            items.Append(row);
            ++added;
        }
        if (added == 0u)
        {
            ListViewItem empty;
            empty.Content(box_value(L"No CRC failures in the last 10 seconds"));
            empty.IsEnabled(false);
            items.Append(empty);
        }
    }

    void MainWindow::RefreshFicBrowser()
    {
        if (!FicFigListView() ||
            !FicFigFilterComboBox() ||
            !FicFigChangedOnlyCheckBox() ||
            !FicFigSearchTextBox())
        {
            return;
        }
        auto items = FicFigListView().Items();
        items.Clear();
        hstring filter{ L"all" };
        auto const filter_item =
            FicFigFilterComboBox().SelectedItem().try_as<ComboBoxItem>();
        if (filter_item && filter_item.Tag())
        {
            filter = unbox_value<hstring>(filter_item.Tag());
        }
        bool const changed_only = FicFigChangedOnlyCheckBox().IsChecked() &&
                                  FicFigChangedOnlyCheckBox().IsChecked().Value();
        std::wstring search{ FicFigSearchTextBox().Text().c_str() };
        std::transform(search.begin(),
                       search.end(),
                       search.begin(),
                       [](wchar_t value) { return std::towlower(value); });

        int32_t selected_row = -1;
        for (uint32_t index = 0u; index < m_fic_fig_stat_count; ++index)
        {
            auto const& stat = m_fic_fig_stats[index];
            if (stat.used == 0u)
            {
                continue;
            }
            bool const malformed = stat.malformed_count != 0u;
            bool const changed = stat.change_count != 0u;
            if ((filter == L"supported" && stat.supported == 0u) ||
                (filter == L"unsupported" && stat.supported != 0u) ||
                (filter == L"malformed" && !malformed) ||
                (filter == L"changed" && !changed) ||
                (changed_only && !changed))
            {
                continue;
            }
            hstring const purpose = fic_fig_purpose(stat.type, stat.extension);
            wchar_t identity[40]{};
            swprintf_s(identity,
                       stat.extension == 0xFFu ? L"FIG %u" : L"FIG %u/%u",
                       stat.type,
                       stat.extension);
            std::wstring searchable = identity;
            searchable += L" ";
            searchable += purpose.c_str();
            std::transform(searchable.begin(),
                           searchable.end(),
                           searchable.begin(),
                           [](wchar_t value) { return std::towlower(value); });
            if (!search.empty() && searchable.find(search) == std::wstring::npos)
            {
                continue;
            }

            StackPanel row;
            row.Orientation(Orientation::Horizontal);
            row.Tag(box_value(index));
            auto append = [&](hstring const& text, double width)
            {
                TextBlock value;
                value.Text(text);
                value.Width(width);
                value.TextTrimming(TextTrimming::CharacterEllipsis);
                row.Children().Append(value);
            };
            append(L"FIG " + to_hstring(stat.type), 90.0);
            append(stat.extension == 0xFFu
                ? hstring{ L"—" }
                : to_hstring(stat.extension), 90.0);
            append(purpose, 330.0);
            size_t const key =
                (static_cast<size_t>(stat.type & 0x07u) << 5u) |
                static_cast<size_t>(stat.extension & 0x1Fu);
            append(format_float(m_fic_fig_rates[key], L"/s", 1), 100.0);
            uint32_t const age_frames =
                m_fic_latest_frame_index >= stat.last_seen_frame
                    ? m_fic_latest_frame_index - stat.last_seen_frame
                    : 0u;
            append(format_float(static_cast<float>(age_frames) * 96.0f,
                                L" ms",
                                0), 110.0);
            append(malformed ? hstring{ L"Malformed" } :
                (stat.supported != 0u ? hstring{ L"Decoded" }
                                     : hstring{ L"Unsupported" }), 100.0);
            if (index == m_fic_selected_fig_index)
            {
                selected_row = static_cast<int32_t>(items.Size());
            }
            items.Append(row);
        }
        if (selected_row >= 0)
        {
            FicFigListView().SelectedIndex(selected_row);
        }
        else if (items.Size() != 0u)
        {
            FicFigListView().SelectedIndex(0);
        }
    }

    void MainWindow::UpdateSelectedFig()
    {
        if (m_fic_selected_fig_index >= m_fic_fig_stat_count)
        {
            return;
        }
        auto const& stat = m_fic_fig_stats[m_fic_selected_fig_index];
        hstring const extension = stat.extension == 0xFFu
            ? hstring{ L"—" }
            : to_hstring(stat.extension);
        FicFigSelectedTitleText().Text(
            L"FIG " + to_hstring(stat.type) + L" / extension " + extension +
            L" · " + fic_fig_purpose(stat.type, stat.extension));
        FicFigSelectedRawText().Text(
            bytes_to_hex(stat.raw, stat.raw_length));
        FicFigSelectedPreviousText().Text(stat.previous_raw_length != 0u
            ? bytes_to_hex(stat.previous_raw, stat.previous_raw_length)
            : hstring{ L"No previous differing occurrence" });

        hstring decoded =
            L"Occurrences: " + to_hstring(stat.total_count) +
            L" · changes: " + to_hstring(stat.change_count) +
            L" · malformed: " + to_hstring(stat.malformed_count) +
            L" · " + (stat.supported != 0u
                ? hstring{ L"decoded by this receiver" }
                : hstring{ L"retained as unsupported raw signalling" });
        if (stat.type == 0u && stat.extension == 0u && stat.raw_length >= 6u)
        {
            uint16_t const eid =
                static_cast<uint16_t>((stat.raw[2] << 8u) | stat.raw[3]);
            uint8_t const change = (stat.raw[4] >> 6u) & 0x03u;
            uint8_t const alarm = (stat.raw[4] >> 5u) & 0x01u;
            uint16_t const cif =
                static_cast<uint16_t>(((stat.raw[4] & 0x1Fu) << 8u) |
                                      stat.raw[5]);
            wchar_t details[160]{};
            swprintf_s(details,
                       L"Ensemble ID 0x%04X · change flag %u · alarm %s · CIF counter %u",
                       eid,
                       change,
                       alarm != 0u ? L"set" : L"not set",
                       cif);
            decoded = details;
        }
        FicFigSelectedDecodedText().Text(decoded);
    }

    void MainWindow::InspectFicHistoryEntry(size_t index)
    {
        if (index >= m_fic_fib_history.size())
        {
            return;
        }
        auto const& entry = m_fic_fib_history[index];
        FicRawTimestampText().Text(history_clock_ms_text(entry.wall_time_100ns));
        FicRawFrameText().Text(
            to_hstring(entry.frame_index) + L" / " +
            to_hstring(entry.fib_index));
        FicRawCrcText().Text(entry.crc_valid ? L"VALID" : L"ERROR");
        FicRawBerText().Text(format_scientific(entry.pre_viterbi_ber));
        FicRawCaptureStateText().Text(L"Frozen from FIB timeline");
        FicRawBytesText().Text(bytes_to_hex(entry.raw.data(),
                                            static_cast<uint32_t>(entry.raw.size())));

        dab_fic_cursor_t cursor{};
        dab_fic_cursor_init(&cursor, entry.raw.data(), 30u);
        std::wstring decoded;
        for (;;)
        {
            dab_fic_fig_t fig{};
            dab_fic_fig_view_t view{};
            int const next = dab_fic_cursor_next_fig(&cursor, &fig);
            if (next == 0)
            {
                break;
            }
            if (next < 0)
            {
                decoded += L"Malformed FIG framing at end of FIB\r\n";
                break;
            }
            uint8_t extension = 0xFFu;
            if (dab_fic_fig_view_from_raw(&fig, &view) == 0)
            {
                if (fig.type == 0u)
                {
                    extension = view.ext;
                }
                else if (fig.type == 1u && fig.length != 0u)
                {
                    extension = fig.payload[0] & 0x07u;
                }
            }
            wchar_t line[160]{};
            if (extension == 0xFFu)
            {
                swprintf_s(line,
                           L"FIG %u · %u bytes · %s\r\n",
                           fig.type,
                           fig.length,
                           fic_fig_purpose(fig.type, extension).c_str());
            }
            else
            {
                swprintf_s(line,
                           L"FIG %u/%u · %u bytes · %s\r\n",
                           fig.type,
                           extension,
                           fig.length,
                           fic_fig_purpose(fig.type, extension).c_str());
            }
            decoded += line;
        }
        FicRawDecodedText().Text(decoded.empty()
            ? hstring{ L"No complete FIG could be decoded from this FIB." }
            : hstring{ decoded });
    }

    void MainWindow::UpdateFicDatabase()
    {
        if (!m_have_catalog)
        {
            return;
        }
        Brush const good = Application::Current().Resources().Lookup(
            box_value(hstring{ L"MeasurementAccentBrush" })).as<Brush>();
        Brush const warning = Application::Current().Resources().Lookup(
            box_value(hstring{ L"WarningBrush" })).as<Brush>();
        bool const consistent = m_latest_catalog.database_consistent != 0u;
        FicDatabaseStatusIndicator().Fill(consistent ? good : warning);
        FicDatabaseStatusText().Foreground(consistent ? good : warning);
        FicDatabaseStatusText().Text(consistent ? L"CONSISTENT" : L"CONFLICTS");

        hstring label = fic_label_text(m_latest_catalog.ensemble_label);
        if (label.empty())
        {
            label = L"Unnamed ensemble";
        }
        wchar_t ensemble[96]{};
        swprintf_s(ensemble,
                   L"%s · ID 0x%04X",
                   label.c_str(),
                   m_latest_catalog.ensemble_id);
        FicDatabaseEnsembleText().Text(ensemble);
        FicDatabaseServicesText().Text(to_hstring(m_latest_catalog.service_count));
        FicDatabaseComponentsText().Text(
            to_hstring(m_latest_catalog.service_component_count));
        FicDatabaseSubchannelsText().Text(
            to_hstring(m_latest_catalog.subchannel_count));
        FicDatabaseRevisionText().Text(to_hstring(m_latest_catalog.sequence));
        FicDatabaseLastChangeText().Text(m_fic_database_last_change);
        FicDatabaseMissingText().Text(
            to_hstring(m_latest_catalog.missing_subchannel_reference_count));
        FicDatabaseDuplicateText().Text(
            to_hstring(m_latest_catalog.duplicate_service_id_count));
        FicDatabaseChangeSummaryText().Text(m_fic_database_change_summary);

        auto items = FicDatabaseListView().Items();
        items.Clear();
        for (uint32_t service_index = 0u;
             service_index < m_latest_catalog.service_count;
             ++service_index)
        {
            auto const& service = m_latest_catalog.services[service_index];
            hstring service_label = fic_label_text(service.label);
            bool duplicate_service_id = false;
            for (uint32_t other_index = 0u;
                 other_index < m_latest_catalog.service_count;
                 ++other_index)
            {
                if (other_index != service_index &&
                    m_latest_catalog.services[other_index].service_id ==
                        service.service_id &&
                    m_latest_catalog.services[other_index].is_programme ==
                        service.is_programme)
                {
                    duplicate_service_id = true;
                    break;
                }
            }
            if (service_label.empty())
            {
                service_label = L"Unnamed service";
            }
            for (uint32_t component_index = 0u;
                 component_index < service.component_count;
                 ++component_index)
            {
                auto const& component = service.components[component_index];
                bool const missing =
                    component.has_subchannel != 0u &&
                    component.subchannel_valid == 0u;
                wchar_t sid[24]{};
                swprintf_s(sid, L"0x%08X", service.service_id);
                StackPanel row;
                row.Orientation(Orientation::Horizontal);
                auto append = [&](hstring const& text,
                                  double width,
                                  Brush const* foreground = nullptr)
                {
                    TextBlock value;
                    value.Text(text);
                    value.Width(width);
                    value.TextTrimming(TextTrimming::CharacterEllipsis);
                    if (foreground != nullptr)
                    {
                        value.Foreground(*foreground);
                    }
                    row.Children().Append(value);
                };
                append(sid,
                       145.0,
                       duplicate_service_id ? &warning : nullptr);
                append(service_label, 330.0);
                append(component_kind_text(component.kind), 130.0);
                append(component.has_subchannel != 0u
                    ? to_hstring(component.subchannel_id)
                    : hstring{ L"—" }, 110.0);
                hstring const status =
                    missing && duplicate_service_id ? hstring{ L"Conflict" } :
                    (missing ? hstring{ L"Missing" } :
                    (duplicate_service_id ? hstring{ L"Duplicate" }
                                          : hstring{ L"Valid" }));
                append(status,
                       100.0,
                       missing || duplicate_service_id ? &warning : &good);
                items.Append(row);
            }
        }
    }

    void MainWindow::ResetFicMeasurementSession()
    {
        m_fic_fib_history.clear();
        m_fic_fig_stats = {};
        m_fic_fig_stat_count = 0u;
        m_fic_selected_fig_index = UINT32_MAX;
        m_fic_timeline_frozen = false;
        m_fic_browser_frozen = false;
        m_fic_last_failure_tick = 0u;
        m_fic_last_total_errors = 0u;
        m_fic_previous_rate_count = 0u;
        m_fic_previous_rate_timestamp_ns = 0u;
        m_fic_rate = 0.0f;
        m_fic_fig_previous_counts = {};
        m_fic_fig_rates = {};
        m_fic_last_seen_change_total = 0u;
        m_fic_change_baseline_valid = false;
        m_fic_latest_frame_index = 0u;
        m_fic_latest_timestamp_ns = 0u;
        m_fic_capture_in_progress = false;
        m_fic_database_last_change = L"—";
        m_fic_database_change_summary = L"No database received";
        FicTimelineResumeButton().Content(box_value(L"Live"));
        FicCrcTimelineCells().Children().Clear();
        PointCollection empty;
        FicViterbiTimelineTrace().Points(empty);
        FicBerTimelineTrace().Points(PointCollection{});
        FicFailureListView().Items().Clear();
        FicFigListView().Items().Clear();
        FicDatabaseListView().Items().Clear();
    }

    dab_receive_subchannel_info_t const* MainWindow::FindMscSubchannel(
        uint8_t subchannel_id) const noexcept
    {
        if (!m_have_catalog)
        {
            return nullptr;
        }
        for (auto const& subchannel : m_latest_catalog.subchannels)
        {
            if (subchannel.valid != 0u &&
                subchannel.subchannel_id == subchannel_id)
            {
                return &subchannel;
            }
        }
        return nullptr;
    }

    dab_receive_service_info_t const* MainWindow::FindMscServiceForSubchannel(
        uint8_t subchannel_id,
        dab_receive_component_info_t const** component) const noexcept
    {
        if (component != nullptr)
        {
            *component = nullptr;
        }
        if (!m_have_catalog)
        {
            return nullptr;
        }
        for (uint32_t service_index = 0u;
             service_index < m_latest_catalog.service_count;
             ++service_index)
        {
            auto const& service = m_latest_catalog.services[service_index];
            for (uint32_t component_index = 0u;
                 component_index < service.component_count;
                 ++component_index)
            {
                auto const& candidate = service.components[component_index];
                if (candidate.has_subchannel != 0u &&
                    candidate.subchannel_id == subchannel_id)
                {
                    if (component != nullptr)
                    {
                        *component = &candidate;
                    }
                    return &service;
                }
            }
        }
        return nullptr;
    }

    void MainWindow::SelectMscSubchannel(uint8_t subchannel_id)
    {
        if (FindMscSubchannel(subchannel_id) == nullptr)
        {
            return;
        }
        if (m_msc_selected_subchannel != subchannel_id)
        {
            m_msc_selected_subchannel = subchannel_id;
            m_msc_measured_output_valid = false;
            m_msc_previous_output_bytes = 0u;
            m_msc_previous_output_timestamp_ns = 0u;
        }
        m_msc_live_selection =
            m_msc_have_snapshot &&
            m_msc_latest_snapshot.msc_selected_subchannel_valid != 0 &&
            m_msc_latest_snapshot.msc_selected_subchannel_id == subchannel_id;
        UpdateMscSelectedDetails();
        RenderMscCapacityMap();
        RefreshMscSubchannelTable();
    }

    void MainWindow::UpdateMscSelectedDetails()
    {
        auto const* subchannel = FindMscSubchannel(m_msc_selected_subchannel);
        if (subchannel == nullptr)
        {
            MscCapacitySelectedText().Text(L"No subchannel selected");
            MscCapacityAddressText().Text(L"—");
            MscCapacitySizeText().Text(L"—");
            MscCapacityBitrateText().Text(L"—");
            MscCapacityProtectionText().Text(L"—");
            MscDetailIdText().Text(L"—");
            MscDetailAddressText().Text(L"—");
            MscDetailCapacityText().Text(L"—");
            MscDetailProtectionText().Text(L"—");
            MscDetailExpectedRateText().Text(L"—");
            MscDetailMeasuredRateText().Text(L"—");
            MscDetailPuncturingText().Text(L"—");
            MscDetailDeinterleaverText().Text(L"—");
            MscDetailViterbiText().Text(L"—");
            MscDetailDecodedText().Text(L"—");
            MscDetailDroppedText().Text(L"—");
            return;
        }

        dab_receive_component_info_t const* component = nullptr;
        auto const* service = FindMscServiceForSubchannel(
            subchannel->subchannel_id,
            &component);
        hstring label = service != nullptr
            ? fic_label_text(service->label)
            : hstring{ L"Unreferenced allocation" };
        if (label.empty())
        {
            label = L"Unnamed service";
        }
        MscCapacitySelectedText().Text(
            label + L" · Subchannel " + to_hstring(subchannel->subchannel_id));
        MscCapacityAddressText().Text(
            to_hstring(subchannel->start_cu) + L" CU");
        MscCapacitySizeText().Text(
            to_hstring(subchannel->size_cu) + L" CU");
        MscCapacityBitrateText().Text(
            subchannel->bitrate_kbps != 0u
                ? to_hstring(subchannel->bitrate_kbps) + L" kbit/s"
                : hstring{ L"Unresolved" });
        MscCapacityProtectionText().Text(msc_protection_text(*subchannel));

        MscDetailIdText().Text(to_hstring(subchannel->subchannel_id));
        MscDetailAddressText().Text(
            to_hstring(subchannel->start_cu) + L" CU");
        MscDetailCapacityText().Text(
            to_hstring(subchannel->size_cu) + L" CU");
        MscDetailProtectionText().Text(msc_protection_text(*subchannel));
        MscDetailExpectedRateText().Text(
            subchannel->bitrate_kbps != 0u
                ? to_hstring(subchannel->bitrate_kbps) + L" kbit/s"
                : hstring{ L"Unresolved" });

        if (!m_msc_live_selection || !m_msc_have_snapshot)
        {
            MscDetailMeasuredRateText().Text(L"Not decoded");
            MscDetailPuncturingText().Text(
                subchannel->allocation_valid != 0u
                    ? hstring{ L"Configuration valid; select service to decode" }
                    : hstring{ L"Invalid allocation" });
            MscDetailDeinterleaverText().Text(L"Not active");
            MscDetailViterbiText().Text(L"Not measured");
            MscDetailDecodedText().Text(L"0");
            MscDetailDroppedText().Text(L"Not measured");
            return;
        }

        auto const& snapshot = m_msc_latest_snapshot;
        MscDetailMeasuredRateText().Text(m_msc_measured_output_valid
            ? format_float(m_msc_measured_output_kbps, L" kbit/s", 3)
            : hstring{ L"Measuring…" });
        MscDetailPuncturingText().Text(snapshot.msc_puncturing_valid != 0u
            ? L"Valid"
            : L"Invalid or unresolved");
        uint64_t const phases =
            (std::min)(snapshot.msc_deinterleaver_cifs_seen, 16ull);
        MscDetailDeinterleaverText().Text(
            phases >= 16u
                ? hstring{ L"Ready · 16/16 phases" }
                : to_hstring(phases) + L"/16 phases");
        MscDetailViterbiText().Text(
            snapshot.msc_input_confidence_samples != 0u
                ? format_float(snapshot.msc_input_confidence_percent, L"%", 1)
                : hstring{ L"—" });
        MscDetailDecodedText().Text(
            to_hstring(snapshot.msc_cif_decode_successes));
        MscDetailDroppedText().Text(
            to_hstring(snapshot.msc_cif_decode_failures) + L" failed · " +
            to_hstring(snapshot.msc_cif_discontinuity_count) +
            L" counter discontinuities");
    }

    void MainWindow::RenderMscCapacityMap()
    {
        if (!MscCapacityCanvas())
        {
            return;
        }
        auto children = MscCapacityCanvas().Children();
        children.Clear();
        double const width = (std::max)(MscCapacityCanvas().ActualWidth(), 900.0);
        constexpr double current_top = 18.0;
        constexpr double current_height = 72.0;

        Microsoft::UI::Xaml::Shapes::Rectangle unused;
        unused.Width(width);
        unused.Height(current_height);
        unused.Fill(SolidColorBrush{ ColorHelper::FromArgb(
            0xFF, 0x14, 0x20, 0x2D) });
        unused.Stroke(SolidColorBrush{ ColorHelper::FromArgb(
            0xFF, 0x3A, 0x4B, 0x5E) });
        Canvas::SetTop(unused, current_top);
        children.Append(unused);

        if (!m_have_catalog)
        {
            MscCapacityCifText().Text(L"CIF —");
            MscCapacityUsageText().Text(L"—");
            MscCapacityConfigSummaryText().Text(
                L"Awaiting FIC subchannel organisation");
            return;
        }

        MscCapacityCifText().Text(
            L"CIF " + to_hstring(m_latest_catalog.cif_count));
        static std::array<Color, 8u> const colours{
            ColorHelper::FromArgb(0xFF, 0x18, 0x8F, 0xA8),
            ColorHelper::FromArgb(0xFF, 0x31, 0x82, 0xCE),
            ColorHelper::FromArgb(0xFF, 0x7C, 0x5C, 0xD6),
            ColorHelper::FromArgb(0xFF, 0x2B, 0xA8, 0x75),
            ColorHelper::FromArgb(0xFF, 0xB7, 0x79, 0x1F),
            ColorHelper::FromArgb(0xFF, 0xB5, 0x4A, 0x6B),
            ColorHelper::FromArgb(0xFF, 0x52, 0x88, 0x45),
            ColorHelper::FromArgb(0xFF, 0x4E, 0x70, 0xA9)
        };

        auto append_allocation =
            [&](dab_receive_subchannel_info_t const& subchannel,
                dab_receive_catalog_t const& catalog,
                double top,
                double height,
                bool interactive)
        {
            if (subchannel.valid == 0u)
            {
                return;
            }
            double const left = width *
                static_cast<double>((std::min)(
                    subchannel.start_cu,
                    static_cast<uint16_t>(DAB_RECEIVE_MSC_CAPACITY_UNITS))) /
                static_cast<double>(DAB_RECEIVE_MSC_CAPACITY_UNITS);
            uint32_t const bounded_end = (std::min)(
                static_cast<uint32_t>(subchannel.start_cu) +
                    static_cast<uint32_t>(subchannel.size_cu),
                static_cast<uint32_t>(DAB_RECEIVE_MSC_CAPACITY_UNITS));
            double const right = width *
                static_cast<double>(bounded_end) /
                static_cast<double>(DAB_RECEIVE_MSC_CAPACITY_UNITS);
            double const segment_width = (std::max)(right - left, 3.0);

            dab_receive_service_info_t const* service = nullptr;
            dab_receive_component_info_t const* component = nullptr;
            for (uint32_t service_index = 0u;
                 service_index < catalog.service_count && service == nullptr;
                 ++service_index)
            {
                auto const& candidate_service = catalog.services[service_index];
                for (uint32_t component_index = 0u;
                     component_index < candidate_service.component_count;
                     ++component_index)
                {
                    auto const& candidate =
                        candidate_service.components[component_index];
                    if (candidate.has_subchannel != 0u &&
                        candidate.subchannel_id == subchannel.subchannel_id)
                    {
                        service = &candidate_service;
                        component = &candidate;
                        break;
                    }
                }
            }
            hstring label = service != nullptr
                ? fic_label_text(service->label)
                : hstring{ L"Unreferenced" };
            if (label.empty())
            {
                label = L"Unnamed service";
            }

            Button segment;
            segment.Tag(box_value(static_cast<uint32_t>(
                subchannel.subchannel_id)));
            segment.Width(segment_width);
            segment.Height(height);
            segment.Padding(Thickness{ 5.0, 2.0, 5.0, 2.0 });
            segment.HorizontalContentAlignment(HorizontalAlignment::Left);
            segment.VerticalContentAlignment(VerticalAlignment::Center);
            bool const allocation_warning =
                subchannel.allocation_valid == 0u ||
                subchannel.overlaps != 0u;
            Color const fill = allocation_warning
                ? ColorHelper::FromArgb(0xFF, 0xB4, 0x2D, 0x3B)
                : colours[subchannel.subchannel_id % colours.size()];
            segment.Background(SolidColorBrush{ fill });
            segment.BorderBrush(SolidColorBrush{
                subchannel.subchannel_id == m_msc_selected_subchannel
                    ? ColorHelper::FromArgb(0xFF, 0xF5, 0xF7, 0xFA)
                    : ColorHelper::FromArgb(0xFF, 0x09, 0x12, 0x1D) });
            double const border_width =
                subchannel.subchannel_id == m_msc_selected_subchannel
                    ? 3.0
                    : 1.0;
            segment.BorderThickness(Thickness{
                border_width, border_width, border_width, border_width });

            TextBlock text;
            text.Text(segment_width >= 95.0
                ? label + L"\nSubch " +
                    to_hstring(subchannel.subchannel_id)
                : L"S" + to_hstring(subchannel.subchannel_id));
            text.TextTrimming(TextTrimming::CharacterEllipsis);
            text.FontSize(interactive ? 11.0 : 9.0);
            segment.Content(text);

            hstring tooltip =
                label + L"\nSubchannel " +
                to_hstring(subchannel.subchannel_id) +
                L" · Address " + to_hstring(subchannel.start_cu) +
                L" CU · Size " + to_hstring(subchannel.size_cu) +
                L" CU\n" + to_hstring(subchannel.bitrate_kbps) +
                L" kbit/s · " + msc_protection_text(subchannel);
            if (allocation_warning)
            {
                tooltip = tooltip + L"\nWarning: invalid or overlapping allocation";
            }
            ToolTipService::SetToolTip(segment, box_value(tooltip));
            if (interactive)
            {
                segment.Click({ this, &MainWindow::MscCapacitySegment_Click });
                segment.DoubleTapped(
                    { this, &MainWindow::MscCapacitySegment_DoubleTapped });
            }
            else
            {
                segment.IsHitTestVisible(false);
                segment.Opacity(0.48);
            }
            Canvas::SetLeft(segment, left);
            Canvas::SetTop(segment, top);
            children.Append(segment);
        };

        for (auto const& subchannel : m_latest_catalog.subchannels)
        {
            append_allocation(
                subchannel,
                m_latest_catalog,
                current_top,
                current_height,
                true);
        }

        auto const compare_checked = MscCompareCapacityToggle().IsChecked();
        bool const compare =
            compare_checked && compare_checked.Value() &&
            m_have_previous_catalog;
        if (compare)
        {
            for (auto const& subchannel : m_previous_catalog.subchannels)
            {
                append_allocation(
                    subchannel,
                    m_previous_catalog,
                    108.0,
                    27.0,
                    false);
            }
            MscCapacityConfigSummaryText().Text(
                L"Current allocation above · previous FIC configuration below");
        }
        else
        {
            MscCapacityConfigSummaryText().Text(
                m_latest_catalog.database_consistent != 0u
                    ? hstring{ L"Allocation is consistent" }
                    : to_hstring(
                        m_latest_catalog.overlapping_subchannel_count) +
                        L" overlapping · " +
                        to_hstring(m_latest_catalog.invalid_subchannel_count) +
                        L" invalid allocation(s)");
        }
        MscCapacityUsageText().Text(
            to_hstring(m_latest_catalog.allocated_capacity_units) +
            L" CU allocated · " +
            to_hstring(m_latest_catalog.unused_capacity_units) +
            L" CU unused");
    }

    void MainWindow::RefreshMscSubchannelTable()
    {
        if (!MscSubchannelListView())
        {
            return;
        }
        auto items = MscSubchannelListView().Items();
        items.Clear();
        if (!m_have_catalog)
        {
            ListViewItem placeholder;
            placeholder.Content(box_value(L"Awaiting FIC subchannel definitions"));
            placeholder.IsEnabled(false);
            items.Append(placeholder);
            return;
        }

        for (auto const& subchannel : m_latest_catalog.subchannels)
        {
            if (subchannel.valid == 0u)
            {
                continue;
            }
            dab_receive_component_info_t const* component = nullptr;
            auto const* service = FindMscServiceForSubchannel(
                subchannel.subchannel_id,
                &component);
            hstring label = service != nullptr
                ? fic_label_text(service->label)
                : hstring{ L"Unreferenced" };
            if (label.empty())
            {
                label = L"Unnamed service";
            }
            bool const live =
                m_msc_live_selection &&
                subchannel.subchannel_id == m_msc_selected_subchannel;
            hstring status = subchannel.allocation_valid == 0u
                ? hstring{ L"Invalid" }
                : (subchannel.overlaps != 0u
                    ? hstring{ L"Overlap" }
                    : (live ? hstring{ L"Decoding" }
                            : hstring{ L"Allocated" }));
            hstring const ber =
                live && m_msc_latest_snapshot.msc_ber_valid != 0
                    ? format_scientific(m_msc_latest_snapshot.msc_ber)
                    : hstring{ L"—" };
            float const buffer_percent =
                live && m_msc_latest_snapshot.msc_dabplus_columns_filled != 0u
                    ? 100.0f *
                        static_cast<float>(
                            m_msc_latest_snapshot.msc_dabplus_columns_filled) /
                        120.0f
                    : 0.0f;

            StackPanel row;
            row.Orientation(Orientation::Horizontal);
            row.Tag(box_value(static_cast<uint32_t>(
                subchannel.subchannel_id)));
            auto append = [&](hstring const& value, double width)
            {
                TextBlock cell;
                cell.Text(value);
                cell.Width(width);
                cell.TextTrimming(TextTrimming::CharacterEllipsis);
                cell.VerticalAlignment(VerticalAlignment::Center);
                row.Children().Append(cell);
            };
            append(to_hstring(subchannel.subchannel_id), 70.0);
            append(label, 330.0);
            append(to_hstring(subchannel.bitrate_kbps), 95.0);
            append(msc_protection_text(subchannel), 100.0);
            append(ber, 120.0);
            append(live
                ? format_float(buffer_percent, L"%", 0)
                : hstring{ L"—" }, 100.0);
            append(status, 125.0);
            items.Append(row);
        }
    }

    void MainWindow::AppendMscHistory(dab_receive_snapshot_t const& snapshot)
    {
        if (!m_msc_live_selection || snapshot.timestamp_ns == 0u)
        {
            return;
        }
        if (m_msc_history_count != 0u)
        {
            size_t const last =
                (m_msc_history_write + msc_history_capacity - 1u) %
                msc_history_capacity;
            if (m_msc_history[last].timestamp_ns == snapshot.timestamp_ns)
            {
                return;
            }
        }
        MscHistorySample sample{};
        sample.timestamp_ns = snapshot.timestamp_ns;
        sample.pre_ber = snapshot.msc_ber;
        sample.ber_valid = snapshot.msc_ber_valid != 0;
        sample.confidence_percent = snapshot.msc_input_confidence_percent;
        sample.confidence_valid =
            snapshot.msc_input_confidence_samples != 0u;
        sample.service_buffer_percent = std::clamp(
            100.0f * static_cast<float>(
                snapshot.msc_dabplus_columns_filled) / 120.0f,
            0.0f,
            100.0f);
        sample.output_kbps = m_msc_measured_output_kbps;
        sample.output_valid = m_msc_measured_output_valid;
        sample.processing_latency_ms =
            static_cast<float>(snapshot.processing_latency_ns) / 1000000.0f;
        sample.live = true;
        m_msc_history[m_msc_history_write] = sample;
        m_msc_history_write =
            (m_msc_history_write + 1u) % msc_history_capacity;
        m_msc_history_count =
            (std::min)(m_msc_history_count + 1u, msc_history_capacity);
    }

    void MainWindow::UpdateMscTimeline()
    {
        if (!MscTimelineCanvas())
        {
            return;
        }
        PointCollection ber;
        PointCollection confidence;
        PointCollection buffer;
        PointCollection output;
        PointCollection latency;
        if (m_msc_history_count == 0u)
        {
            MscTimelineBerTrace().Points(ber);
            MscTimelineConfidenceTrace().Points(confidence);
            MscTimelineBufferTrace().Points(buffer);
            MscTimelineOutputTrace().Points(output);
            MscTimelineLatencyTrace().Points(latency);
            return;
        }

        uint64_t period_seconds = 60u;
        auto const period_item =
            MscTimelinePeriodComboBox().SelectedItem().try_as<ComboBoxItem>();
        if (period_item && period_item.Tag())
        {
            period_seconds = std::wcstoull(
                unbox_value<hstring>(period_item.Tag()).c_str(),
                nullptr,
                10);
        }
        size_t const first_ring =
            (m_msc_history_write + msc_history_capacity -
             m_msc_history_count) % msc_history_capacity;
        size_t const latest_ring =
            (m_msc_history_write + msc_history_capacity - 1u) %
            msc_history_capacity;
        uint64_t const latest_ns = m_msc_history[latest_ring].timestamp_ns;
        uint64_t const start_ns =
            latest_ns > period_seconds * 1000000000ull
                ? latest_ns - period_seconds * 1000000000ull
                : 0u;
        double const width = (std::max)(
            MscTimelineCanvas().ActualWidth(), 900.0);
        double const height = (std::max)(
            MscTimelineCanvas().ActualHeight(), 245.0);
        auto enabled = [](CheckBox const& check_box)
        {
            auto const value = check_box.IsChecked();
            return value && value.Value();
        };
        bool const show_ber = enabled(MscTimelineBerCheckBox());
        bool const show_confidence =
            enabled(MscTimelineConfidenceCheckBox());
        bool const show_buffer = enabled(MscTimelineBufferCheckBox());
        bool const show_output = enabled(MscTimelineOutputCheckBox());
        bool const show_latency = enabled(MscTimelineLatencyCheckBox());
        float const expected_kbps =
            m_msc_latest_snapshot.msc_decoder_bitrate_kbps != 0u
                ? static_cast<float>(
                    m_msc_latest_snapshot.msc_decoder_bitrate_kbps)
                : 192.0f;

        for (size_t offset = 0u; offset < m_msc_history_count; ++offset)
        {
            auto const& sample = m_msc_history[
                (first_ring + offset) % msc_history_capacity];
            if (sample.timestamp_ns < start_ns)
            {
                continue;
            }
            double const x = period_seconds != 0u
                ? width * std::clamp(
                    static_cast<double>(sample.timestamp_ns - start_ns) /
                        static_cast<double>(
                            period_seconds * 1000000000ull),
                    0.0,
                    1.0)
                : 0.0;
            if (show_ber && sample.ber_valid)
            {
                float const log_ber = std::log10(
                    std::clamp(sample.pre_ber, 1.0e-7f, 1.0e-1f));
                double const normalized =
                    std::clamp(static_cast<double>(
                        (log_ber + 7.0f) / 6.0f), 0.0, 1.0);
                ber.Append(Point{
                    static_cast<float>(x),
                    static_cast<float>(height * (1.0 - normalized)) });
            }
            if (show_confidence && sample.confidence_valid)
            {
                confidence.Append(Point{
                    static_cast<float>(x),
                    static_cast<float>(
                        height * (1.0 - std::clamp(
                            sample.confidence_percent / 100.0f,
                            0.0f,
                            1.0f))) });
            }
            if (show_buffer)
            {
                buffer.Append(Point{
                    static_cast<float>(x),
                    static_cast<float>(
                        height * (1.0 - std::clamp(
                            sample.service_buffer_percent / 100.0f,
                            0.0f,
                            1.0f))) });
            }
            if (show_output && sample.output_valid)
            {
                output.Append(Point{
                    static_cast<float>(x),
                    static_cast<float>(
                        height * (1.0 - std::clamp(
                            sample.output_kbps / expected_kbps,
                            0.0f,
                            1.2f) / 1.2f)) });
            }
            if (show_latency)
            {
                latency.Append(Point{
                    static_cast<float>(x),
                    static_cast<float>(
                        height * (1.0 - std::clamp(
                            sample.processing_latency_ms / 20.0f,
                            0.0f,
                            1.0f))) });
            }
        }
        MscTimelineBerTrace().Points(ber);
        MscTimelineConfidenceTrace().Points(confidence);
        MscTimelineBufferTrace().Points(buffer);
        MscTimelineOutputTrace().Points(output);
        MscTimelineLatencyTrace().Points(latency);
    }

    void MainWindow::UpdateMscAlarms(
        dab_receive_snapshot_t const& snapshot)
    {
        auto alarms = MscAlarmListView().Items();
        alarms.Clear();
        auto append_alarm = [&](hstring const& value)
        {
            TextBlock item;
            item.Text(L"⚠ " + value);
            item.TextWrapping(TextWrapping::Wrap);
            item.Foreground(Application::Current().Resources().Lookup(
                box_value(hstring{ L"WarningBrush" })).as<Brush>());
            alarms.Append(item);
        };

        bool const new_decode_failure =
            snapshot.msc_cif_decode_failures >
            m_msc_last_cif_failure_count;
        bool const new_discontinuity =
            snapshot.msc_cif_discontinuity_count >
            m_msc_last_cif_discontinuity_count;
        if (m_msc_live_selection &&
            snapshot.msc_ber_valid != 0 &&
            snapshot.msc_ber > 1.0e-3f)
        {
            append_alarm(
                L"Pre-Viterbi pseudo-BER above 1 × 10⁻³");
        }
        if (m_msc_live_selection &&
            snapshot.msc_input_confidence_samples != 0u &&
            snapshot.msc_input_confidence_percent < 55.0f)
        {
            append_alarm(L"Viterbi input confidence below 55%");
        }
        if (m_msc_live_selection &&
            snapshot.msc_frames_seen >= 16u &&
            snapshot.msc_deinterleaver_cifs_seen < 16u)
        {
            append_alarm(L"Time deinterleaver is not ready");
        }
        if (new_decode_failure)
        {
            append_alarm(L"Subchannel CIF decode failure observed");
        }
        if (new_discontinuity)
        {
            append_alarm(L"FIC-signalled CIF counter discontinuity");
        }
        if (m_msc_live_selection &&
            m_msc_measured_output_valid &&
            snapshot.msc_decoder_bitrate_kbps != 0u)
        {
            float const expected =
                static_cast<float>(snapshot.msc_decoder_bitrate_kbps);
            if (std::abs(m_msc_measured_output_kbps - expected) >
                expected * 0.02f)
            {
                append_alarm(L"Measured output bitrate differs by more than 2%");
            }
        }
        if (snapshot.processing_late_count != 0u)
        {
            append_alarm(
                to_hstring(snapshot.processing_late_count) +
                L" receiver processing-late block(s)");
        }
        if (snapshot.dropped_blocks != 0u)
        {
            append_alarm(
                to_hstring(snapshot.dropped_blocks) +
                L" dropped SDR input block(s)");
        }
        if (m_have_catalog &&
            m_latest_catalog.overlapping_subchannel_count != 0u)
        {
            append_alarm(
                to_hstring(
                    m_latest_catalog.overlapping_subchannel_count) +
                L" overlapping subchannel allocation(s)");
        }
        if (m_have_catalog &&
            m_latest_catalog.invalid_subchannel_count != 0u)
        {
            append_alarm(
                to_hstring(m_latest_catalog.invalid_subchannel_count) +
                L" invalid subchannel allocation(s)");
        }
        if (m_msc_selected_subchannel != 0xFFu &&
            FindMscSubchannel(m_msc_selected_subchannel) == nullptr)
        {
            append_alarm(L"Selected service/subchannel disappeared");
        }

        if (alarms.Size() == 0u)
        {
            TextBlock clear;
            clear.Text(L"No current MSC alarms");
            clear.Foreground(Application::Current().Resources().Lookup(
                box_value(hstring{ L"MutedTextBrush" })).as<Brush>());
            alarms.Append(clear);
        }

        auto events = MscErrorEventList().Items();
        auto append_event = [&](hstring const& description)
        {
            TextBlock event;
            event.Text(local_clock_text() + L" · Warning · " + description);
            event.TextWrapping(TextWrapping::Wrap);
            events.Append(event);
            while (events.Size() > 50u)
            {
                events.RemoveAt(0u);
            }
        };
        if (new_decode_failure)
        {
            append_event(
                L"CIF decode failures +" +
                to_hstring(snapshot.msc_cif_decode_failures -
                           m_msc_last_cif_failure_count));
        }
        if (new_discontinuity)
        {
            append_event(
                L"FIC CIF counter discontinuities +" +
                to_hstring(snapshot.msc_cif_discontinuity_count -
                           m_msc_last_cif_discontinuity_count));
        }
        m_msc_last_cif_failure_count =
            snapshot.msc_cif_decode_failures;
        m_msc_last_cif_discontinuity_count =
            snapshot.msc_cif_discontinuity_count;
    }

    void MainWindow::ResetMscMeasurementSession()
    {
        m_previous_catalog = {};
        m_have_previous_catalog = false;
        m_msc_selected_subchannel = 0xFFu;
        m_msc_latest_snapshot = {};
        m_msc_have_snapshot = false;
        m_msc_live_selection = false;
        m_msc_measured_output_kbps = 0.0f;
        m_msc_measured_output_valid = false;
        m_msc_previous_output_bytes = 0u;
        m_msc_previous_output_timestamp_ns = 0u;
        m_msc_max_processing_latency_ns = 0u;
        m_msc_last_cif_failure_count = 0u;
        m_msc_last_cif_discontinuity_count = 0u;
        m_msc_history = {};
        m_msc_history_write = 0u;
        m_msc_history_count = 0u;
        if (MscErrorEventList())
        {
            MscErrorEventList().Items().Clear();
        }
        if (MscAlarmListView())
        {
            MscAlarmListView().Items().Clear();
        }
        if (MscCapacityCanvas())
        {
            RenderMscCapacityMap();
        }
        if (MscSubchannelListView())
        {
            RefreshMscSubchannelTable();
        }
        UpdateMscTimeline();
        UpdateMscSelectedDetails();
    }

    void MainWindow::ApplyMscSnapshot(
        dab_receive_snapshot_t const& snapshot)
    {
        bool const followed_live_selection = m_msc_live_selection;
        uint8_t const previous_subchannel = m_msc_selected_subchannel;
        m_msc_latest_snapshot = snapshot;
        m_msc_have_snapshot = true;

        if (snapshot.msc_selected_subchannel_valid != 0 &&
            FindMscSubchannel(snapshot.msc_selected_subchannel_id) != nullptr &&
            (m_msc_selected_subchannel == 0xFFu ||
             (followed_live_selection &&
              previous_subchannel != snapshot.msc_selected_subchannel_id)))
        {
            m_msc_selected_subchannel =
                snapshot.msc_selected_subchannel_id;
            m_msc_previous_output_bytes = 0u;
            m_msc_previous_output_timestamp_ns = 0u;
            m_msc_measured_output_valid = false;
        }
        m_msc_live_selection =
            snapshot.msc_selected_subchannel_valid != 0 &&
            snapshot.msc_selected_subchannel_id ==
                m_msc_selected_subchannel;

        if (m_msc_live_selection)
        {
            if (m_msc_previous_output_timestamp_ns != 0u &&
                snapshot.timestamp_ns >
                    m_msc_previous_output_timestamp_ns &&
                snapshot.msc_output_bytes >=
                    m_msc_previous_output_bytes)
            {
                double const seconds =
                    static_cast<double>(
                        snapshot.timestamp_ns -
                        m_msc_previous_output_timestamp_ns) /
                    1000000000.0;
                float const instantaneous =
                    static_cast<float>(
                        static_cast<double>(
                            snapshot.msc_output_bytes -
                            m_msc_previous_output_bytes) *
                        8.0 / 1000.0 / seconds);
                if (std::isfinite(instantaneous))
                {
                    m_msc_measured_output_kbps =
                        m_msc_measured_output_valid
                            ? 0.75f * m_msc_measured_output_kbps +
                                0.25f * instantaneous
                            : instantaneous;
                    m_msc_measured_output_valid = true;
                }
            }
            m_msc_previous_output_bytes = snapshot.msc_output_bytes;
            m_msc_previous_output_timestamp_ns = snapshot.timestamp_ns;
        }

        m_msc_max_processing_latency_ns = (std::max)(
            m_msc_max_processing_latency_ns,
            snapshot.processing_latency_ns);

        auto const* subchannel =
            FindMscSubchannel(m_msc_selected_subchannel);
        dab_receive_component_info_t const* component = nullptr;
        auto const* service = subchannel != nullptr
            ? FindMscServiceForSubchannel(
                subchannel->subchannel_id,
                &component)
            : nullptr;
        hstring service_label = service != nullptr
            ? fic_label_text(service->label)
            : hstring{ L"No service" };
        if (service_label.empty())
        {
            service_label = L"Unnamed service";
        }
        if (subchannel != nullptr)
        {
            MscHeaderContextText().Text(
                L"Service: " + service_label +
                L" · Subchannel " +
                to_hstring(subchannel->subchannel_id) +
                L" · " + to_hstring(subchannel->bitrate_kbps) +
                L" kbit/s · " + msc_protection_text(*subchannel));
        }
        else
        {
            MscHeaderContextText().Text(
                L"Awaiting selected service and subchannel");
        }

        hstring status{ L"IDLE" };
        hstring brush_name{ L"MutedTextBrush" };
        if (m_msc_live_selection && snapshot.msc_synchronized != 0)
        {
            status = snapshot.msc_deinterleaver_cifs_seen >= 16u
                ? hstring{ L"DECODING" }
                : hstring{ L"FILLING" };
            brush_name = L"MeasurementAccentBrush";
        }
        else if (subchannel != nullptr)
        {
            status = L"ALLOCATED · NOT DECODED";
            brush_name = L"WarningBrush";
        }
        MscHeaderStatusText().Text(status);
        Brush const status_brush =
            Application::Current().Resources().Lookup(
                box_value(brush_name)).as<Brush>();
        MscHeaderStatusText().Foreground(status_brush);
        MscHeaderStatusIndicator().Fill(status_brush);

        bool const have_live_data =
            m_msc_live_selection &&
            snapshot.msc_cif_decode_attempts != 0u;
        MscPreBerValueText().Text(
            m_msc_live_selection && snapshot.msc_ber_valid != 0
                ? format_scientific(snapshot.msc_ber)
                : hstring{ L"—" });
        MscPreBerDetailText().Text(!m_msc_live_selection
            ? hstring{ L"Selected subchannel is not being decoded" }
            : (snapshot.msc_ber_valid != 0
                ? hstring{ L"Re-encoded coded-bit comparison" }
                : hstring{ L"Awaiting compared coded bits" }));

        bool const downstream_validated =
            m_msc_live_selection &&
            snapshot.msc_last_decode_ok != 0;
        MscPostViterbiValueText().Text(downstream_validated
            ? L"VALIDATED"
            : L"UNVERIFIED");
        MscPostViterbiDetailText().Text(downstream_validated
            ? L"DAB+ outer checks passed for latest superframe"
            : L"No direct residual-error detector in convolutional decoder");

        MscOutputRateValueText().Text(
            m_msc_live_selection && m_msc_measured_output_valid
                ? format_float(m_msc_measured_output_kbps, L" kbit/s", 1)
                : hstring{ L"—" });
        MscOutputRateDetailText().Text(
            subchannel != nullptr && subchannel->bitrate_kbps != 0u
                ? L"Expected " +
                    to_hstring(subchannel->bitrate_kbps) + L" kbit/s"
                : hstring{ L"Expected rate unresolved" });

        float const continuity =
            have_live_data
                ? 100.0f *
                    static_cast<float>(
                        static_cast<double>(
                            snapshot.msc_cif_decode_successes) /
                        static_cast<double>(
                            snapshot.msc_cif_decode_attempts))
                : 0.0f;
        MscContinuityValueText().Text(have_live_data
            ? format_float(continuity, L"%", 2)
            : hstring{ L"—" });
        MscContinuityDetailText().Text(have_live_data
            ? (snapshot.msc_cif_decode_failures == 0u
                ? hstring{ L"No CIF decode failures" }
                : to_hstring(snapshot.msc_cif_decode_failures) +
                    L" CIF decode failure(s)")
            : hstring{ L"Awaiting decoded CIF output" });

        MscViterbiQualityValueText().Text(
            m_msc_live_selection &&
            snapshot.msc_input_confidence_samples != 0u
                ? format_float(
                    snapshot.msc_input_confidence_percent,
                    L"%",
                    1)
                : hstring{ L"—" });
        MscViterbiQualityDetailText().Text(
            snapshot.msc_input_confidence_samples != 0u
                ? (snapshot.msc_input_confidence_percent >= 70.0f
                    ? hstring{ L"Good softbit confidence" }
                    : hstring{ L"Low softbit confidence" })
                : hstring{ L"Awaiting Viterbi input softbits" });

        uint64_t const deinterleaver_phases = (std::min)(
            snapshot.msc_deinterleaver_cifs_seen,
            16ull);
        MscDeinterleaverValueText().Text(
            m_msc_live_selection && deinterleaver_phases >= 16u
                ? L"READY"
                : (m_msc_live_selection
                    ? hstring{ L"FILLING" }
                    : hstring{ L"—" }));
        MscDeinterleaverDetailText().Text(
            m_msc_live_selection
                ? to_hstring(deinterleaver_phases) + L"/16 phases"
                : hstring{ L"Selected allocation is not active" });

        float const service_buffer_percent = std::clamp(
            100.0f * static_cast<float>(
                snapshot.msc_dabplus_columns_filled) / 120.0f,
            0.0f,
            100.0f);
        MscBufferValueText().Text(m_msc_live_selection
            ? format_float(service_buffer_percent, L"%", 0)
            : hstring{ L"—" });
        MscBufferDetailText().Text(m_msc_live_selection
            ? to_hstring(snapshot.msc_dabplus_columns_filled) +
                L"/120 DAB+ superframe columns"
            : hstring{ L"Selected allocation is not active" });

        MscCifStatusValueText().Text(
            m_msc_live_selection &&
            snapshot.msc_latest_cif_valid_mask == 0x0Fu
                ? L"VALID"
                : (m_msc_live_selection
                    ? hstring{ L"PARTIAL" }
                    : hstring{ L"—" }));
        MscCifStatusDetailText().Text(
            L"FIC counter " + to_hstring(snapshot.fic_cif_count) +
            L" · " +
            to_hstring(snapshot.msc_cif_discontinuity_count) +
            L" discontinuities");

        uint32_t valid_cifs = 0u;
        for (uint32_t bit = 0u; bit < 4u; ++bit)
        {
            valid_cifs +=
                (snapshot.msc_latest_cif_valid_mask >> bit) & 1u;
        }
        MscBufferInputValueText().Text(
            to_hstring(valid_cifs) + L"/4 latest");
        MscBufferInputStateText().Text(m_msc_live_selection
            ? (valid_cifs == 4u ? hstring{ L"Complete" }
                                : hstring{ L"Partial frame" })
            : hstring{ L"Not active" });
        MscBufferTimeValueText().Text(format_float(
            100.0f * static_cast<float>(deinterleaver_phases) / 16.0f,
            L"%",
            0));
        MscBufferTimeStateText().Text(
            deinterleaver_phases >= 16u ? L"Ready" : L"Filling");
        MscBufferViterbiStateText().Text(
            snapshot.msc_puncturing_valid != 0u
                ? L"Synchronous · puncturing valid"
                : L"Awaiting valid puncturing");
        MscBufferDecodedValueText().Text(
            to_hstring(snapshot.msc_cif_decode_successes) + L" CIFs");
        MscBufferDecodedStateText().Text(
            snapshot.msc_cif_decode_failures == 0u
                ? L"No decode failures"
                : to_hstring(snapshot.msc_cif_decode_failures) +
                    L" failure(s)");
        MscBufferServiceValueText().Text(
            to_hstring(snapshot.msc_dabplus_columns_filled) + L"/120");
        MscBufferServiceStateText().Text(
            snapshot.msc_audio_valid != 0
                ? L"DAB+ component active"
                : L"No DAB+ component output");
        MscBufferLatencyText().Text(format_float(
            static_cast<float>(m_msc_max_processing_latency_ns) /
                1000000.0f,
            L" ms",
            2));
        MscBufferLateText().Text(
            to_hstring(snapshot.processing_late_count));
        MscBufferDroppedText().Text(
            to_hstring(snapshot.dropped_blocks));
        MscBufferCifGapText().Text(
            to_hstring(snapshot.msc_cif_discontinuity_count) +
            L" FIC-signalled");

        if (m_msc_live_selection)
        {
            wchar_t mask[16]{};
            swprintf_s(
                mask,
                L"0x%02X",
                snapshot.msc_latest_cif_valid_mask);
            MscRawSubchannelText().Text(
                to_hstring(snapshot.msc_selected_subchannel_id));
            MscRawValidMaskText().Text(mask);
            MscRawCifBytesText().Text(
                to_hstring(snapshot.msc_latest_cif_bytes[0]) + L" / " +
                to_hstring(snapshot.msc_latest_cif_bytes[1]) + L" / " +
                to_hstring(snapshot.msc_latest_cif_bytes[2]) + L" / " +
                to_hstring(snapshot.msc_latest_cif_bytes[3]) + L" bytes");
            MscRawBytesText().Text(
                bytes_to_hex_lines(
                    snapshot.msc_raw_data,
                    snapshot.msc_raw_data_length));
        }
        else
        {
            MscRawSubchannelText().Text(L"Not decoded");
            MscRawValidMaskText().Text(L"—");
            MscRawCifBytesText().Text(L"—");
            MscRawBytesText().Text(
                L"Double-click this allocation in Capacity Map to select "
                L"its service and begin decoding.");
        }

        AppendMscHistory(snapshot);
        UpdateMscSelectedDetails();
        RefreshMscSubchannelTable();
        UpdateMscTimeline();
        UpdateMscAlarms(snapshot);
    }

    void MainWindow::ApplyDabSnapshot(dab_receive_snapshot_t const& snapshot)
    {
        UpdateDabContext();
        ApplyFicSnapshot(snapshot);
        ApplyMscSnapshot(snapshot);

        bool const locked = snapshot.locked != 0;
        DabModeBadgeText().Text(L"● LIVE");
        DabSyncBadgeText().Text(locked ? L"● SYNCED" : L"ACQUIRING");
        DabSyncBadgeText().Foreground(Application::Current().Resources().Lookup(
            box_value(hstring{ locked
                ? L"MeasurementAccentBrush"
                : L"WarningBrush" })).as<Brush>());

        constexpr uint32_t fibs_per_frame = 12u;
        bool const have_fic = locked && snapshot.fic_frames_seen != 0u;
        double const fic_quality = have_fic
            ? std::clamp(static_cast<double>(snapshot.fic_total_valid_fibs) /
                         (static_cast<double>(snapshot.fic_frames_seen) * fibs_per_frame),
                         0.0,
                         1.0)
            : 0.0;
        uint64_t const total_fibs =
            static_cast<uint64_t>(snapshot.fic_frames_seen) * fibs_per_frame;
        uint64_t const fic_crc_errors = total_fibs >= snapshot.fic_total_valid_fibs
            ? total_fibs - snapshot.fic_total_valid_fibs
            : 0u;
        hstring const fic_quality_text = have_fic
            ? format_float(static_cast<float>(fic_quality * 100.0), L"%")
            : hstring{ L"—" };
        DabOverviewFicCrcValueText().Text(have_fic
            ? format_float(static_cast<float>(fic_quality * 100.0), L"% valid", 2)
            : hstring{ L"—" });
        DabOverviewFicCrcDetailText().Text(have_fic
            ? to_hstring(fic_crc_errors) +
                (fic_crc_errors == 1u ? L" CRC error" : L" CRC errors")
            : hstring{ L"Awaiting decoded FIBs" });
        DabOverviewFicValueText().Text(fic_quality_text);
        DabOverviewFicDetailText().Text(have_fic
            ? to_hstring(snapshot.fic_valid_fibs_last_frame) + L" / 12 valid FIBs in latest frame"
            : hstring{ L"Awaiting synchronized FIC" });
        DabOverviewFicFramesText().Text(to_hstring(snapshot.fic_frames_seen));
        DabFicFramesText().Text(to_hstring(snapshot.fic_frames_seen));
        DabFicBlocksText().Text(to_hstring(snapshot.fic_blocks_decoded_last_frame));
        DabFicValidText().Text(to_hstring(snapshot.fic_valid_fibs_last_frame) + L" / 12");
        DabFicQualityText().Text(fic_quality_text);
        DabFicTotalText().Text(to_hstring(snapshot.fic_total_valid_fibs));

        hstring const msc_state = snapshot.msc_synchronized != 0 ? L"SYNCED" : L"ACQUIRING";
        hstring const superframe_state = snapshot.msc_superframe_valid != 0 ? L"VALID" : L"INVALID";
        hstring const audio_state = snapshot.msc_audio_valid != 0 ? L"AVAILABLE" : L"WAITING";
        DabOverviewMscValueText().Text(msc_state);
        DabOverviewMscDetailText().Text(to_hstring(snapshot.msc_frames_seen) + L" MSC frames processed");
        DabOverviewMscFramesText().Text(to_hstring(snapshot.msc_frames_seen));
        DabMscFramesText().Text(to_hstring(snapshot.msc_frames_seen));
        DabMscSyncText().Text(msc_state);
        DabMscSuperframeText().Text(superframe_state);
        DabMscAudioText().Text(audio_state);
        DabMscGenerationText().Text(to_hstring(snapshot.msc_superframe_generation));

        hstring const ber_value = snapshot.msc_ber_valid != 0
            ? format_float(snapshot.msc_ber, L"", 6)
            : hstring{ L"—" };
        hstring const ber_detail = snapshot.msc_ber_valid != 0
            ? (snapshot.msc_ber < 1.0e-3f ? hstring{ L"Stable · " } : hstring{ L"Degraded · " }) +
                to_hstring(snapshot.msc_ber_error_bits) + L" errors / " +
                to_hstring(snapshot.msc_ber_compared_bits) + L" compared bits"
            : hstring{ L"Awaiting decoded and re-encoded MSC bits" };
        DabOverviewMscBerValueText().Text(ber_value);
        DabOverviewMscBerDetailText().Text(ber_detail);
        DabMscBerPageValueText().Text(ber_value);
        DabMscBerPageDetailText().Text(ber_detail);

        bool const have_attempt = snapshot.msc_decode_attempts != 0u;
        uint64_t const successful_decodes =
            snapshot.msc_decode_attempts >= snapshot.msc_decode_failures
                ? snapshot.msc_decode_attempts - snapshot.msc_decode_failures
                : 0u;
        hstring const success_rate = have_attempt
            ? format_float(100.0f * static_cast<float>(successful_decodes) /
                           static_cast<float>(snapshot.msc_decode_attempts),
                           L"%")
            : hstring{ L"—" };
        hstring dab_state{ L"WAITING" };
        if (snapshot.msc_audio_valid != 0)
        {
            dab_state = snapshot.msc_last_decode_ok != 0 ? L"DECODING" : L"ACQUIRING";
        }
        hstring const last_decode = !have_attempt
            ? hstring{ L"No decode attempt" }
            : (snapshot.msc_last_decode_ok != 0
                ? hstring{ L"Latest decode successful" }
                : hstring{ L"Latest decode failed" });

        if (snapshot.msc_superframe_generation != 0u)
        {
            double const rs_average =
                static_cast<double>(snapshot.msc_rs_corrected_total) /
                static_cast<double>(snapshot.msc_superframe_generation);
            DabOverviewRsValueText().Text(
                format_float(static_cast<float>(rs_average), L" bytes/frame", 1));
            m_dab_rs_peak = (std::max)(m_dab_rs_peak,
                                       snapshot.msc_rs_corrected_last_superframe);
            DabOverviewRsDetailText().Text(
                L"Peak: " + to_hstring(m_dab_rs_peak) +
                L" · latest: " + to_hstring(snapshot.msc_rs_corrected_last_superframe));
        }
        else
        {
            DabOverviewRsValueText().Text(L"—");
            DabOverviewRsDetailText().Text(L"No valid superframes");
        }

        uint64_t const unavailable_superframes = snapshot.msc_decode_failures;
        DabOverviewAudioAvailabilityValueText().Text(have_attempt
            ? format_float(100.0f * static_cast<float>(successful_decodes) /
                           static_cast<float>(snapshot.msc_decode_attempts),
                           L"%",
                           2)
            : hstring{ L"—" });
        DabOverviewAudioAvailabilityDetailText().Text(have_attempt
            ? to_hstring(unavailable_superframes * 120u) + L" ms unavailable"
            : hstring{ L"Awaiting DAB+ superframes" });

        if (m_dab_previous_fig_timestamp_ns != 0u &&
            snapshot.timestamp_ns > m_dab_previous_fig_timestamp_ns &&
            snapshot.fic_total_figs >= m_dab_previous_fig_count)
        {
            double const elapsed_seconds =
                static_cast<double>(snapshot.timestamp_ns - m_dab_previous_fig_timestamp_ns) /
                1000000000.0;
            float const instantaneous_rate = static_cast<float>(
                static_cast<double>(snapshot.fic_total_figs - m_dab_previous_fig_count) /
                elapsed_seconds);
            m_dab_fig_rate = m_dab_fig_rate == 0.0f
                ? instantaneous_rate
                : 0.75f * m_dab_fig_rate + 0.25f * instantaneous_rate;
        }
        m_dab_previous_fig_count = snapshot.fic_total_figs;
        m_dab_previous_fig_timestamp_ns = snapshot.timestamp_ns;
        DabOverviewFigValueText().Text(
            to_hstring(snapshot.fic_total_fig_parse_errors) +
            (snapshot.fic_total_fig_parse_errors == 1u ? L" error" : L" errors"));
        DabOverviewFigDetailText().Text(snapshot.fic_total_figs != 0u
            ? format_float(m_dab_fig_rate, L" FIG/s", 1) + L" · " +
                to_hstring(snapshot.fic_total_figs) + L" parsed"
            : hstring{ L"Awaiting FIC" });

        if (snapshot.msc_au_crc_errors_total > m_dab_last_audio_crc_errors)
        {
            m_dab_last_audio_crc_error_tick = m_tick;
        }
        m_dab_last_audio_crc_errors = snapshot.msc_au_crc_errors_total;
        DabOverviewAudioCrcValueText().Text(snapshot.msc_au_crc_checks_total != 0u
            ? to_hstring(snapshot.msc_au_crc_errors_total) + L" errors"
            : hstring{ L"—" });
        if (snapshot.msc_au_crc_checks_total == 0u)
        {
            DabOverviewAudioCrcDetailText().Text(L"Awaiting audio units");
        }
        else if (m_dab_last_audio_crc_error_tick == 0u)
        {
            DabOverviewAudioCrcDetailText().Text(
                to_hstring(snapshot.msc_au_crc_checks_total) + L" units checked · no errors");
        }
        else
        {
            uint64_t const elapsed_seconds =
                (m_tick - m_dab_last_audio_crc_error_tick) / 4u;
            DabOverviewAudioCrcDetailText().Text(
                L"Last: " + format_duration(elapsed_seconds * 1000000000ull) + L" ago");
        }

        if (m_have_catalog)
        {
            uint64_t const stable_seconds =
                m_tick >= m_dab_catalog_changed_tick
                    ? (m_tick - m_dab_catalog_changed_tick) / 4u
                    : 0u;
            DabOverviewReconfigValueText().Text(L"NONE PENDING");
            hstring detail =
                L"Database stable " + format_duration(stable_seconds * 1000000000ull);
            if (snapshot.msc_last_reconfig_reason != 0u)
            {
                wchar_t reason[32]{};
                swprintf_s(reason,
                           L" · last applied 0x%02X",
                           snapshot.msc_last_reconfig_reason);
                detail = detail + reason;
            }
            DabOverviewReconfigDetailText().Text(detail);
        }
        else
        {
            DabOverviewReconfigValueText().Text(L"WAITING");
            DabOverviewReconfigDetailText().Text(L"Awaiting FIC database");
        }

        DabOverviewDabValueText().Text(dab_state);
        DabOverviewDabDetailText().Text(last_decode);
        DabOverviewGenerationText().Text(to_hstring(snapshot.msc_superframe_generation));
        DabOverviewResetText().Text(to_hstring(snapshot.msc_runtime_reset_count));
        DabOverviewStatusValueText().Text(locked ? L"SYNCED" : L"ACQUIRING");
        DabOverviewStatusDetailText().Text(L"Snapshot " + to_hstring(snapshot.sequence));

        DabPlusStateText().Text(dab_state);
        DabPlusLastDecodeText().Text(last_decode);
        DabPlusSuccessRateText().Text(success_rate);
        DabPlusRsLastText().Text(to_hstring(snapshot.msc_rs_corrected_last_superframe));
        DabPlusResetsText().Text(to_hstring(snapshot.msc_runtime_reset_count));
        DabPlusGenerationText().Text(to_hstring(snapshot.msc_superframe_generation));
        DabPlusAttemptsText().Text(to_hstring(snapshot.msc_decode_attempts));
        DabPlusFailuresText().Text(to_hstring(snapshot.msc_decode_failures));
        DabPlusRsTotalText().Text(to_hstring(snapshot.msc_rs_corrected_total));
        DabPlusAudioCrcText().Text(snapshot.msc_au_crc_checks_total != 0u
            ? to_hstring(snapshot.msc_au_crc_errors_total) + L" errors / " +
                to_hstring(snapshot.msc_au_crc_checks_total) + L" units"
            : hstring{ L"Awaiting audio units" });
        DabPlusAvailabilityText().Text(have_attempt
            ? success_rate + L" · " + to_hstring(unavailable_superframes * 120u) +
                L" ms unavailable"
            : hstring{ L"Awaiting superframes" });
        DabPlusLastDecodeDetailText().Text(last_decode);
        wchar_t reconfiguration[32]{};
        swprintf_s(reconfiguration, L"Reason mask 0x%02X", snapshot.msc_last_reconfig_reason);
        DabPlusReconfigText().Text(reconfiguration);

        ApplyDabPlusSnapshot(snapshot);
        UpdateDabPipeline(snapshot);
        AppendDabOverviewHistory(snapshot);
    }

    void MainWindow::ApplyDabPlusSnapshot(
        dab_receive_snapshot_t const& snapshot)
    {
        m_dabplus_latest_snapshot = snapshot;
        m_dabplus_have_snapshot = true;

        hstring context = DabContextText().Text();
        if (snapshot.audio_format_valid != 0)
        {
            context = context + L" · " +
                to_hstring(snapshot.audio_sample_rate_hz / 1000u) +
                L" kHz · " +
                (snapshot.audio_channels == 1u
                    ? hstring{ L"Mono" }
                    : hstring{ L"Stereo" });
        }
        DabPlusHeaderContextText().Text(context);
        DabPlusLiveBadgeText().Text(m_metrics_stale ? L"STALE" : L"● LIVE");
        DabPlusAudioBadgeText().Text(snapshot.audio_output_active != 0
            ? L"● AUDIO VALID"
            : (snapshot.msc_superframe_valid != 0
                ? hstring{ L"AUDIO BUFFERING" }
                : hstring{ L"AUDIO WAITING" }));

        auto percent_text = [](uint64_t good, uint64_t total)
        {
            return total != 0u
                ? format_float(
                    100.0f * static_cast<float>(good) /
                        static_cast<float>(total),
                    L"% valid",
                    2)
                : hstring{ L"—" };
        };

        uint64_t const attempts = snapshot.msc_decode_attempts;
        uint64_t invalid = snapshot.msc_decode_failures;
        invalid += snapshot.msc_rs_uncorrectable_total;
        invalid = (std::min)(invalid, attempts);
        uint64_t const valid_superframes = attempts - invalid;
        DabPlusSuperframeValidValueText().Text(
            percent_text(valid_superframes, attempts));
        DabPlusSuperframeValidDetailText().Text(attempts != 0u
            ? to_hstring(valid_superframes) + L" / " +
                to_hstring(attempts)
            : hstring{ L"Awaiting frames" });

        uint64_t const fire_checks = snapshot.msc_firecode_checks_total;
        uint64_t const fire_errors = (std::min)(
            static_cast<uint64_t>(snapshot.msc_firecode_errors_total),
            fire_checks);
        DabPlusFireCodeValueText().Text(
            percent_text(fire_checks - fire_errors, fire_checks));
        DabPlusFireCodeDetailText().Text(fire_checks != 0u
            ? to_hstring(fire_errors) +
                (fire_errors == 1u ? L" failure" : L" failures")
            : hstring{ L"Awaiting checks" });

        if (snapshot.msc_superframe_generation != 0u)
        {
            DabPlusRsMeanValueText().Text(format_float(
                static_cast<float>(snapshot.msc_rs_corrected_total) /
                    static_cast<float>(snapshot.msc_superframe_generation),
                L" bytes/frame",
                1));
            DabPlusRsMeanDetailText().Text(
                L"Peak " + to_hstring(snapshot.msc_rs_corrected_max) +
                L" · latest " +
                to_hstring(snapshot.msc_rs_corrected_last_superframe));
        }
        else
        {
            DabPlusRsMeanValueText().Text(L"—");
            DabPlusRsMeanDetailText().Text(L"Awaiting frames");
        }
        DabPlusRsUncorrectableValueText().Text(
            to_hstring(snapshot.msc_rs_uncorrectable_total));

        uint64_t const au_checks = snapshot.msc_au_crc_checks_total;
        uint64_t const au_errors = (std::min)(
            static_cast<uint64_t>(snapshot.msc_au_crc_errors_total),
            au_checks);
        DabPlusAuCrcValueText().Text(
            percent_text(au_checks - au_errors, au_checks));
        DabPlusAuCrcDetailText().Text(au_checks != 0u
            ? to_hstring(au_errors) + L" failures"
            : hstring{ L"Awaiting units" });
        DabPlusAacErrorValueText().Text(
            to_hstring(snapshot.audio_aac_errors));
        DabPlusAacErrorDetailText().Text(
            snapshot.audio_aac_errors == 0u &&
            snapshot.audio_au_decoded != 0u
                ? hstring{ L"Decoder healthy" }
                : to_hstring(snapshot.audio_au_decoded) +
                    L" units decoded");

        uint64_t muted_ms = 0u;
        uint64_t longest_ms = 0u;
        if (snapshot.audio_sample_rate_hz != 0u)
        {
            muted_ms = (snapshot.audio_muted_frames * 1000u) /
                snapshot.audio_sample_rate_hz;
            longest_ms =
                (snapshot.audio_longest_interruption_frames * 1000u) /
                snapshot.audio_sample_rate_hz;
        }
        DabPlusMuteValueText().Text(
            to_hstring(muted_ms) + L" ms");
        DabPlusMuteDetailText().Text(
            L"Longest " + to_hstring(longest_ms) +
            L" ms · concealment unavailable");
        DabPlusAudioAvailabilityValueText().Text(
            snapshot.audio_output_frames != 0u
                ? format_float(snapshot.audio_availability_percent, L"%", 2)
                : hstring{ L"—" });
        DabPlusAudioAvailabilityDetailText().Text(
            to_hstring(snapshot.audio_underrun_events) +
            L" output underruns");

        if (snapshot.audio_level_valid != 0)
        {
            DabPlusAudioLeftRmsText().Text(
                L"Left RMS          " +
                format_float(snapshot.audio_left_rms_dbfs, L" dBFS", 1));
            DabPlusAudioRightRmsText().Text(
                L"Right RMS         " +
                format_float(snapshot.audio_right_rms_dbfs, L" dBFS", 1));
            DabPlusAudioLeftPeakText().Text(
                L"Left peak         " +
                format_float(snapshot.audio_left_peak_dbfs, L" dBFS", 1));
            DabPlusAudioRightPeakText().Text(
                L"Right peak        " +
                format_float(snapshot.audio_right_peak_dbfs, L" dBFS", 1));
        }
        else
        {
            DabPlusAudioLeftRmsText().Text(L"Left RMS          —");
            DabPlusAudioRightRmsText().Text(L"Right RMS         —");
            DabPlusAudioLeftPeakText().Text(L"Left peak         —");
            DabPlusAudioRightPeakText().Text(L"Right peak        —");
        }
        DabPlusAudioClippingText().Text(
            L"Audio clipping    " +
            to_hstring(snapshot.audio_clipped_samples) + L" · " +
            format_float(snapshot.audio_clipping_percent, L"%", 4));
        DabPlusAudioValidText().Text(
            L"Valid audio       " +
            (snapshot.audio_output_frames != 0u
                ? format_float(snapshot.audio_availability_percent, L"%", 2)
                : hstring{ L"—" }));
        DabPlusAudioMutedText().Text(
            L"Muted audio       " + to_hstring(muted_ms) + L" ms");
        DabPlusAudioLongestMuteText().Text(
            L"Longest interruption " + to_hstring(longest_ms) + L" ms");
        DabPlusAudioUnderrunsText().Text(
            L"Output underruns  " +
            to_hstring(snapshot.audio_underrun_events));
        DabPlusAudioQueueDropsText().Text(
            L"Queue drops       " +
            to_hstring(snapshot.audio_queue_drops));

        hstring const mode = snapshot.audio_channels == 1u
            ? L"Mono"
            : (snapshot.audio_channels == 2u
                ? hstring{ L"Stereo" }
                : hstring{ L"—" });
        DabPlusAudioFormatText().Text(
            L"Format " + to_hstring(snapshot.audio_sample_rate_hz) +
            L" Hz · " + mode +
            (snapshot.msc_dabplus_sbr_flag != 0u
                ? hstring{ L" · SBR" }
                : hstring{ L" · no SBR" }) +
            (snapshot.msc_dabplus_ps_flag != 0u
                ? hstring{ L" · PS" }
                : hstring{}));
        hstring decoder{ L"Not configured" };
        if (snapshot.audio_decoder_backend == DAB_AUDIO_DECODER_FAAD)
        {
            decoder = L"FAAD2";
        }
        else if (snapshot.audio_decoder_backend == DAB_AUDIO_DECODER_FDK)
        {
            decoder = L"FDK-AAC";
        }
        DabPlusAudioDecoderText().Text(
            L"Decoder " + decoder +
            (snapshot.audio_output_active != 0
                ? hstring{ L" · output active" }
                : hstring{ L" · output inactive" }));
        DabPlusAudioCountsText().Text(
            L"Audio units " + to_hstring(snapshot.audio_au_received) +
            L" · decoded " + to_hstring(snapshot.audio_au_decoded) +
            L" · CRC rejected " +
            to_hstring(snapshot.audio_au_crc_rejected) +
            L" · AAC errors " + to_hstring(snapshot.audio_aac_errors) +
            L" · output errors " +
            to_hstring(snapshot.audio_output_errors));
        DabPlusPadDetectedText().Text(
            to_hstring(snapshot.pad_access_units) + L" detected");
        DabPlusPadErrorsText().Text(
            to_hstring(snapshot.pad_parse_errors));
        DabPlusPadSupportText().Text(
            snapshot.pad_metadata_decoder_supported != 0u
                ? hstring{ L"Enabled" }
                : hstring{ L"Not implemented in the measurement receiver" });

        uint32_t latest_attempt = 0u;
        if (snapshot.msc_dabplus_history_count != 0u)
        {
            latest_attempt = snapshot.msc_dabplus_history[
                snapshot.msc_dabplus_history_count - 1u].attempt_number;
        }
        if (latest_attempt != m_dabplus_last_attempt ||
            (latest_attempt == 0u &&
             snapshot.msc_dabplus_history_count == 0u))
        {
            m_dabplus_last_attempt = latest_attempt;
            RefreshDabPlusSuperframes();
            UpdateDabPlusRsVisuals();
        }

        uint64_t latest_audio_generation = 0u;
        uint8_t latest_audio_index = 0u;
        if (snapshot.audio_au_history_count != 0u)
        {
            auto const& latest = snapshot.audio_au_history[
                snapshot.audio_au_history_count - 1u];
            latest_audio_generation = latest.generation;
            latest_audio_index = latest.au_index;
        }
        if (latest_audio_generation != m_dabplus_last_audio_generation ||
            latest_audio_index != m_dabplus_last_audio_index)
        {
            m_dabplus_last_audio_generation = latest_audio_generation;
            m_dabplus_last_audio_index = latest_audio_index;
            RefreshDabPlusAudioUnits();
        }
        UpdateDabPlusAlarms();
    }

    void MainWindow::RefreshDabPlusSuperframes()
    {
        DabPlusSuperframeList().Items().Clear();
        DabPlusSuperframeTimelinePanel().Children().Clear();
        if (!m_dabplus_have_snapshot)
        {
            return;
        }
        auto const& snapshot = m_dabplus_latest_snapshot;
        uint32_t const first = snapshot.msc_dabplus_history_count > 100u
            ? snapshot.msc_dabplus_history_count - 100u
            : 0u;
        Brush const valid_brush = Application::Current().Resources().Lookup(
            box_value(hstring{ L"MeasurementAccentBrush" })).as<Brush>();
        Brush const corrected_brush = Application::Current().Resources().Lookup(
            box_value(hstring{ L"SpectrumBrush" })).as<Brush>();
        Brush const warning_brush = Application::Current().Resources().Lookup(
            box_value(hstring{ L"WarningBrush" })).as<Brush>();
        for (uint32_t index = first;
             index < snapshot.msc_dabplus_history_count;
             ++index)
        {
            auto const& frame = snapshot.msc_dabplus_history[index];
            uint32_t valid_aus = 0u;
            for (uint32_t au = 0u; au < frame.au_count; ++au)
            {
                if ((frame.au_crc_valid_mask & (1u << au)) != 0u)
                {
                    valid_aus++;
                }
            }
            hstring state = frame.superframe_valid != 0u
                ? L"VALID"
                : (frame.firecode_valid == 0u
                    ? hstring{ L"FIRE ERROR" }
                    : (frame.rs_uncorrectable != 0u
                        ? hstring{ L"RS UNCORRECTABLE" }
                        : hstring{ L"INVALID" }));
            hstring row = L"#" + to_hstring(frame.generation) +
                L"   " + state +
                L"   RS " + to_hstring(frame.rs_corrected_bytes) +
                L" bytes   AU " + to_hstring(valid_aus) + L"/" +
                to_hstring(frame.au_count) +
                L"   sample " + to_hstring(frame.frame_start_sample);
            DabPlusSuperframeList().Items().Append(box_value(row));

            Border marker;
            marker.Width(7.0);
            marker.Height(frame.superframe_valid != 0u ? 24.0 : 34.0);
            marker.CornerRadius(CornerRadius{ 2.0 });
            marker.Background(frame.superframe_valid == 0u
                ? warning_brush
                : (frame.rs_corrected_bytes != 0u
                    ? corrected_brush
                    : valid_brush));
            DabPlusSuperframeTimelinePanel().Children().Append(marker);
        }
        if (snapshot.msc_dabplus_history_count != 0u)
        {
            DabPlusSuperframeList().SelectedIndex(
                static_cast<int32_t>(
                    snapshot.msc_dabplus_history_count - first - 1u));
        }
    }

    void MainWindow::UpdateDabPlusSelectedSuperframe()
    {
        if (!m_dabplus_have_snapshot)
        {
            return;
        }
        auto const& snapshot = m_dabplus_latest_snapshot;
        uint32_t const first = snapshot.msc_dabplus_history_count > 100u
            ? snapshot.msc_dabplus_history_count - 100u
            : 0u;
        int32_t const selected = DabPlusSuperframeList().SelectedIndex();
        if (selected < 0 ||
            first + static_cast<uint32_t>(selected) >=
                snapshot.msc_dabplus_history_count)
        {
            return;
        }
        auto const& frame = snapshot.msc_dabplus_history[
            first + static_cast<uint32_t>(selected)];
        uint32_t valid_aus = 0u;
        for (uint32_t au = 0u; au < frame.au_count; ++au)
        {
            valid_aus +=
                (frame.au_crc_valid_mask & (1u << au)) != 0u ? 1u : 0u;
        }
        DabPlusSelectedSuperframeText().Text(
            L"#" + to_hstring(frame.generation) +
            L" · attempt " + to_hstring(frame.attempt_number));
        DabPlusSelectedFireRsText().Text(
            (frame.firecode_valid != 0u ? hstring{ L"Fire valid" }
                                        : hstring{ L"Fire failed" }) +
            L" · " + to_hstring(frame.rs_corrected_bytes) +
            L" corrected · " +
            to_hstring(frame.rs_uncorrectable_rows) +
            L" uncorrectable rows");
        DabPlusSelectedAuText().Text(
            to_hstring(valid_aus) + L" / " +
            to_hstring(frame.au_count) + L" CRC valid");
        DabPlusSelectedImpactText().Text(
            frame.superframe_valid != 0u && valid_aus == frame.au_count
                ? hstring{ L"None recorded" }
                : hstring{ L"AU rejected or audio muted" });
        DabPlusSelectedSampleText().Text(
            to_hstring(frame.frame_start_sample));
    }

    void MainWindow::RefreshDabPlusAudioUnits()
    {
        DabPlusAudioUnitList().Items().Clear();
        if (!m_dabplus_have_snapshot)
        {
            return;
        }
        auto const& snapshot = m_dabplus_latest_snapshot;
        for (uint32_t index = 0u;
             index < snapshot.audio_au_history_count;
             ++index)
        {
            auto const& au = snapshot.audio_au_history[index];
            hstring result{ L"Pending" };
            switch (au.decoder_result)
            {
            case DAB_RECEIVE_AUDIO_AU_DECODED:
                result = L"Decoded";
                break;
            case DAB_RECEIVE_AUDIO_AU_CRC_REJECTED:
                result = L"CRC rejected";
                break;
            case DAB_RECEIVE_AUDIO_AU_DECODE_ERROR:
                result = L"AAC error";
                break;
            case DAB_RECEIVE_AUDIO_AU_OUTPUT_ERROR:
                result = L"Output error";
                break;
            default:
                break;
            }
            DabPlusAudioUnitList().Items().Append(box_value(
                L"SF " + to_hstring(au.generation) +
                L" · AU " + to_hstring(au.au_index) +
                L" · " + to_hstring(au.length) +
                L" bytes · " + result +
                (au.pad_present != 0u ? hstring{ L" · PAD" } : hstring{})));
        }
        if (snapshot.audio_au_history_count != 0u)
        {
            DabPlusAudioUnitList().SelectedIndex(
                static_cast<int32_t>(
                    snapshot.audio_au_history_count - 1u));
        }
    }

    void MainWindow::UpdateDabPlusSelectedAudioUnit()
    {
        if (!m_dabplus_have_snapshot)
        {
            return;
        }
        int32_t const selected = DabPlusAudioUnitList().SelectedIndex();
        auto const& snapshot = m_dabplus_latest_snapshot;
        if (selected < 0 ||
            static_cast<uint32_t>(selected) >=
                snapshot.audio_au_history_count)
        {
            return;
        }
        auto const& au =
            snapshot.audio_au_history[static_cast<uint32_t>(selected)];
        hstring result{ L"Pending" };
        switch (au.decoder_result)
        {
        case DAB_RECEIVE_AUDIO_AU_DECODED:
            result = L"Valid";
            break;
        case DAB_RECEIVE_AUDIO_AU_CRC_REJECTED:
            result = L"Not decoded (CRC rejected)";
            break;
        case DAB_RECEIVE_AUDIO_AU_DECODE_ERROR:
            result = L"AAC decoder error";
            break;
        case DAB_RECEIVE_AUDIO_AU_OUTPUT_ERROR:
            result = L"Audio output error";
            break;
        default:
            break;
        }
        DabPlusAuDetailIdentityText().Text(
            L"Superframe " + to_hstring(au.generation) +
            L" · AU " + to_hstring(au.au_index) +
            L" · " + to_hstring(au.length) + L" bytes");
        DabPlusAuDetailOffsetsText().Text(
            L"Start " + to_hstring(au.start_offset) +
            L" · end " + to_hstring(au.start_offset + au.length));
        DabPlusAuDetailCrcText().Text(
            au.crc_valid != 0u ? L"CRC valid" : L"CRC error");
        DabPlusAuDetailDecoderText().Text(
            L"AAC decode: " + result);
        DabPlusAuDetailFormatText().Text(
            (snapshot.msc_dabplus_sbr_flag != 0u
                ? hstring{ L"SBR enabled" }
                : hstring{ L"SBR disabled" }) +
            L" · " +
            (snapshot.msc_dabplus_ps_flag != 0u
                ? hstring{ L"PS enabled" }
                : hstring{ L"PS disabled" }) +
            L" · concealment not implemented");
        DabPlusAuDetailRawText().Text(
            au.preview_length != 0u
                ? bytes_to_hex(au.preview, au.preview_length)
                : hstring{ L"—" });
    }

    void MainWindow::UpdateDabPlusRsVisuals()
    {
        if (!m_dabplus_have_snapshot)
        {
            return;
        }
        auto const& snapshot = m_dabplus_latest_snapshot;
        uint32_t buckets[5]{};
        uint32_t uncorrectable = 0u;
        PointCollection points;
        double const width = DabPlusRsHistoryCanvas().ActualWidth() > 1.0
            ? DabPlusRsHistoryCanvas().ActualWidth()
            : 620.0;
        double const height = 300.0;
        uint32_t const count = snapshot.msc_dabplus_history_count;
        uint32_t const first = count > 100u ? count - 100u : 0u;
        uint32_t const visible = count - first;
        uint32_t max_value = 1u;
        for (uint32_t index = first; index < count; ++index)
        {
            max_value = (std::max)(
                max_value,
                static_cast<uint32_t>(
                    snapshot.msc_dabplus_history[index].rs_corrected_bytes));
        }
        for (uint32_t index = first; index < count; ++index)
        {
            auto const& frame = snapshot.msc_dabplus_history[index];
            uint32_t bucket = frame.rs_corrected_bytes == 0u ? 0u
                : (frame.rs_corrected_bytes <= 2u ? 1u
                : (frame.rs_corrected_bytes <= 5u ? 2u
                : (frame.rs_corrected_bytes <= 10u ? 3u : 4u)));
            buckets[bucket]++;
            uncorrectable += frame.rs_uncorrectable != 0u ? 1u : 0u;
            double const x = visible > 1u
                ? width * static_cast<double>(index - first) /
                    static_cast<double>(visible - 1u)
                : 0.0;
            double const y = height - 8.0 -
                (height - 16.0) *
                    static_cast<double>(frame.rs_corrected_bytes) /
                    static_cast<double>(max_value);
            points.Append(Point{
                static_cast<float>(x),
                static_cast<float>(y)
            });
        }
        DabPlusRsHistoryPolyline().Points(points);

        std::array<TextBlock, 5> labels{
            DabPlusRsDistributionZeroText(),
            DabPlusRsDistributionLowText(),
            DabPlusRsDistributionMediumText(),
            DabPlusRsDistributionHighText(),
            DabPlusRsDistributionSevereText()
        };
        std::array<ProgressBar, 5> bars{
            DabPlusRsDistributionZeroBar(),
            DabPlusRsDistributionLowBar(),
            DabPlusRsDistributionMediumBar(),
            DabPlusRsDistributionHighBar(),
            DabPlusRsDistributionSevereBar()
        };
        wchar_t const* names[5]{
            L"0 bytes", L"1–2 bytes", L"3–5 bytes",
            L"6–10 bytes", L">10 bytes"
        };
        for (uint32_t index = 0u; index < 5u; ++index)
        {
            float const percent = visible != 0u
                ? 100.0f * static_cast<float>(buckets[index]) /
                    static_cast<float>(visible)
                : 0.0f;
            labels[index].Text(
                hstring{ names[index] } + L"   " +
                format_float(percent, L"%", 1));
            bars[index].Value(percent);
        }
        DabPlusRsSummaryText().Text(
            L"Mean corrected bytes " +
            (snapshot.msc_superframe_generation != 0u
                ? format_float(
                    static_cast<float>(snapshot.msc_rs_corrected_total) /
                        static_cast<float>(
                            snapshot.msc_superframe_generation),
                    L"",
                    2)
                : hstring{ L"—" }) +
            L" · maximum " + to_hstring(snapshot.msc_rs_corrected_max) +
            L" · corrected superframes " +
            to_hstring(snapshot.msc_rs_corrected_superframes) +
            L" · uncorrectable " +
            to_hstring(snapshot.msc_rs_uncorrectable_total) +
            L" (visible " + to_hstring(uncorrectable) + L")");
    }

    void MainWindow::UpdateDabPlusAlarms()
    {
        DabPlusAlarmList().Items().Clear();
        if (!m_dabplus_have_snapshot)
        {
            return;
        }
        auto const& snapshot = m_dabplus_latest_snapshot;
        auto add_alarm = [&](hstring const& text)
        {
            DabPlusAlarmList().Items().Append(
                box_value(L"⚠ " + text));
        };
        if (snapshot.msc_firecode_errors_total != 0u)
        {
            add_alarm(L"Fire-code failures: " +
                      to_hstring(snapshot.msc_firecode_errors_total));
        }
        if (snapshot.msc_rs_corrected_last_superframe > 10u)
        {
            add_alarm(L"High RS corrections in latest frame: " +
                      to_hstring(
                          snapshot.msc_rs_corrected_last_superframe) +
                      L" bytes");
        }
        if (snapshot.msc_rs_uncorrectable_total != 0u)
        {
            add_alarm(L"RS uncorrectable superframes: " +
                      to_hstring(snapshot.msc_rs_uncorrectable_total));
        }
        if (snapshot.msc_au_crc_errors_total != 0u)
        {
            add_alarm(L"Audio-unit CRC failures: " +
                      to_hstring(snapshot.msc_au_crc_errors_total));
        }
        if (snapshot.audio_aac_errors != 0u)
        {
            add_alarm(L"AAC decoder failures: " +
                      to_hstring(snapshot.audio_aac_errors));
        }
        if (snapshot.audio_output_frames != 0u &&
            snapshot.audio_availability_percent < 99.0f)
        {
            add_alarm(L"Audio availability below 99%: " +
                      format_float(
                          snapshot.audio_availability_percent,
                          L"%",
                          2));
        }
        if (snapshot.audio_clipped_samples != 0u)
        {
            add_alarm(L"Audio PCM clipping: " +
                      to_hstring(snapshot.audio_clipped_samples) +
                      L" samples");
        }
        if (snapshot.pad_parse_errors != 0u)
        {
            add_alarm(L"Malformed PAD framing: " +
                      to_hstring(snapshot.pad_parse_errors));
        }
        if (DabPlusAlarmList().Items().Size() == 0u)
        {
            DabPlusAlarmList().Items().Append(
                box_value(L"No current DAB+ warnings"));
        }
    }

    void MainWindow::UpdateDabPipeline(dab_receive_snapshot_t const& snapshot)
    {
        Brush const active_brush = Application::Current().Resources().Lookup(
            box_value(hstring{ L"MeasurementAccentBrush" })).as<Brush>();
        Brush const waiting_brush = Application::Current().Resources().Lookup(
            box_value(hstring{ L"WarningBrush" })).as<Brush>();
        Brush const inactive_brush = Application::Current().Resources().Lookup(
            box_value(hstring{ L"MutedTextBrush" })).as<Brush>();

        bool const rf_valid = snapshot.locked != 0;
        bool const fic_valid = rf_valid && snapshot.fic_valid_fibs_last_frame != 0u;
        bool const msc_valid = fic_valid && snapshot.msc_synchronized != 0;
        bool const dab_valid = msc_valid &&
                               snapshot.msc_superframe_valid != 0 &&
                               snapshot.msc_last_decode_ok != 0;
        bool const audio_playing = dab_valid && m_receiver.AudioOutputActive();

        auto set_stage = [&](winrt::Microsoft::UI::Xaml::Shapes::Ellipse const& indicator,
                             TextBlock const& state,
                             bool valid,
                             bool upstream_valid,
                             hstring const& valid_text,
                             hstring const& waiting_text)
        {
            Brush const brush = valid
                ? active_brush
                : (upstream_valid ? waiting_brush : inactive_brush);
            indicator.Fill(brush);
            state.Foreground(brush);
            state.Text(valid ? valid_text : waiting_text);
        };

        set_stage(DabPipelineRfIndicator(),
                  DabPipelineRfStateText(),
                  rf_valid,
                  true,
                  L"LOCKED",
                  L"ACQUIRING");
        set_stage(DabPipelineFicIndicator(),
                  DabPipelineFicStateText(),
                  fic_valid,
                  rf_valid,
                  L"VALID",
                  L"WAITING");
        set_stage(DabPipelineMscIndicator(),
                  DabPipelineMscStateText(),
                  msc_valid,
                  fic_valid,
                  L"VALID",
                  L"WAITING");
        set_stage(DabPipelineDabIndicator(),
                  DabPipelineDabStateText(),
                  dab_valid,
                  msc_valid,
                  L"VALID",
                  L"WAITING");
        set_stage(DabPipelineAudioIndicator(),
                  DabPipelineAudioStateText(),
                  audio_playing,
                  dab_valid,
                  L"PLAYING",
                  dab_valid ? hstring{ L"BUFFERING" } : hstring{ L"STOPPED" });
    }

    void MainWindow::AppendDabOverviewHistory(dab_receive_snapshot_t const& snapshot)
    {
        constexpr float unavailable = std::numeric_limits<float>::quiet_NaN();
        float fic_crc_percent = unavailable;
        if (snapshot.fic_frames_seen != 0u)
        {
            constexpr float fibs_per_frame = 12.0f;
            fic_crc_percent = std::clamp(
                100.0f * static_cast<float>(snapshot.fic_total_valid_fibs) /
                    (static_cast<float>(snapshot.fic_frames_seen) * fibs_per_frame),
                0.0f,
                100.0f);
        }

        size_t const slot = m_dab_overview_history_write;
        m_dab_fic_crc_history[slot] = fic_crc_percent;
        m_dab_msc_ber_history[slot] = snapshot.msc_ber_valid != 0
            ? snapshot.msc_ber
            : unavailable;
        m_dab_rs_history[slot] = snapshot.msc_superframe_generation != 0u
            ? static_cast<float>(snapshot.msc_rs_corrected_last_superframe)
            : unavailable;
        m_dab_audio_history[slot] = snapshot.msc_last_decode_ok != 0 ? 1u : 0u;
        m_dab_failure_history[slot] = snapshot.msc_decode_failures;
        m_dab_overview_history_write =
            (slot + 1u) % dab_overview_history_capacity;
        m_dab_overview_history_count =
            (std::min)(m_dab_overview_history_count + 1u,
                       dab_overview_history_capacity);

        UpdateDabOverviewHistoryVisuals();
    }

    void MainWindow::UpdateDabOverviewHistoryVisuals()
    {
        constexpr float left = 70.0f;
        constexpr float width = 825.0f;
        PointCollection fic_points;
        PointCollection ber_points;
        PointCollection rs_points;
        PointCollection audio_points;
        if (m_dab_overview_history_count == 0u)
        {
            DabOverviewFicTimelineTrace().Points(fic_points);
            DabOverviewMscTimelineTrace().Points(ber_points);
            DabOverviewRsTimelineTrace().Points(rs_points);
            DabOverviewAudioTimelineTrace().Points(audio_points);
            return;
        }

        size_t const first =
            (m_dab_overview_history_write + dab_overview_history_capacity -
             m_dab_overview_history_count) %
            dab_overview_history_capacity;
        for (size_t point = 0u; point < m_dab_overview_history_count; ++point)
        {
            size_t const index = (first + point) % dab_overview_history_capacity;
            float const x = m_dab_overview_history_count > 1u
                ? left + width * static_cast<float>(point) /
                    static_cast<float>(m_dab_overview_history_count - 1u)
                : left + width;

            float const fic = m_dab_fic_crc_history[index];
            if (std::isfinite(fic))
            {
                float const y = 28.0f -
                    std::clamp((fic - 80.0f) / 20.0f, 0.0f, 1.0f) * 17.0f;
                fic_points.Append(Point{ x, y });
            }

            float const ber = m_dab_msc_ber_history[index];
            if (std::isfinite(ber))
            {
                float const log_ber = std::log10(
                    std::clamp(ber, 1.0e-7f, 1.0e-1f));
                float const normalized = std::clamp(
                    (-1.0f - log_ber) / 6.0f,
                    0.0f,
                    1.0f);
                ber_points.Append(Point{ x, 61.0f - normalized * 17.0f });
            }

            float const rs = m_dab_rs_history[index];
            if (std::isfinite(rs))
            {
                rs_points.Append(Point{
                    x,
                    94.0f - std::clamp(rs / 20.0f, 0.0f, 1.0f) * 17.0f
                });
            }

            bool const audio_available = m_dab_audio_history[index] != 0u;
            audio_points.Append(Point{ x, audio_available ? 112.0f : 127.0f });
        }

        DabOverviewFicTimelineTrace().Points(fic_points);
        DabOverviewMscTimelineTrace().Points(ber_points);
        DabOverviewRsTimelineTrace().Points(rs_points);
        DabOverviewAudioTimelineTrace().Points(audio_points);
        size_t const last =
            (m_dab_overview_history_write + dab_overview_history_capacity - 1u) %
            dab_overview_history_capacity;
        uint32_t const first_failures = m_dab_failure_history[first];
        uint32_t const last_failures = m_dab_failure_history[last];
        uint32_t const recent_failures = last_failures >= first_failures
            ? last_failures - first_failures
            : 0u;
        DabOverviewMuteMarkerText().Text(recent_failures == 0u
            ? hstring{ L"No superframe interruptions" }
            : to_hstring(static_cast<uint64_t>(recent_failures) * 120u) +
                L" ms unavailable");
    }

    void MainWindow::ResetDabOverviewHistory()
    {
        float const unavailable = std::numeric_limits<float>::quiet_NaN();
        m_dab_fic_crc_history.fill(unavailable);
        m_dab_msc_ber_history.fill(unavailable);
        m_dab_rs_history.fill(unavailable);
        m_dab_audio_history = {};
        m_dab_failure_history = {};
        m_dab_overview_history_write = 0u;
        m_dab_overview_history_count = 0u;
        m_dab_rs_peak = 0u;
        m_dab_previous_fig_count = 0u;
        m_dab_previous_fig_timestamp_ns = 0u;
        m_dab_fig_rate = 0.0f;
        m_dab_catalog_changed_tick = m_tick;
        m_dab_last_audio_crc_errors = 0u;
        m_dab_last_audio_crc_error_tick = 0u;
        m_dabplus_latest_snapshot = {};
        m_dabplus_have_snapshot = false;
        m_dabplus_last_attempt = 0u;
        m_dabplus_last_audio_generation = 0u;
        m_dabplus_last_audio_index = 0u;
        if (DabPlusSuperframeList())
        {
            DabPlusSuperframeList().Items().Clear();
            DabPlusSuperframeTimelinePanel().Children().Clear();
            DabPlusAudioUnitList().Items().Clear();
            DabPlusAlarmList().Items().Clear();
        }
        DabOverviewMuteMarkerText().Text(L"No superframe interruptions");
        UpdateDabOverviewHistoryVisuals();

        Brush const inactive_brush = Application::Current().Resources().Lookup(
            box_value(hstring{ L"MutedTextBrush" })).as<Brush>();
        DabPipelineRfIndicator().Fill(inactive_brush);
        DabPipelineFicIndicator().Fill(inactive_brush);
        DabPipelineMscIndicator().Fill(inactive_brush);
        DabPipelineDabIndicator().Fill(inactive_brush);
        DabPipelineAudioIndicator().Fill(inactive_brush);
        DabPipelineRfStateText().Foreground(inactive_brush);
        DabPipelineFicStateText().Foreground(inactive_brush);
        DabPipelineMscStateText().Foreground(inactive_brush);
        DabPipelineDabStateText().Foreground(inactive_brush);
        DabPipelineAudioStateText().Foreground(inactive_brush);
        DabPipelineRfStateText().Text(L"IDLE");
        DabPipelineFicStateText().Text(L"WAITING");
        DabPipelineMscStateText().Text(L"WAITING");
        DabPipelineDabStateText().Text(L"WAITING");
        DabPipelineAudioStateText().Text(L"STOPPED");
    }

    void MainWindow::UpdateCpuUsage()
    {
        FILETIME creation{};
        FILETIME exit{};
        FILETIME process_kernel{};
        FILETIME process_user{};
        FILETIME system_idle{};
        FILETIME system_kernel{};
        FILETIME system_user{};
        if (!GetProcessTimes(GetCurrentProcess(),
                             &creation,
                             &exit,
                             &process_kernel,
                             &process_user) ||
            !GetSystemTimes(&system_idle, &system_kernel, &system_user))
        {
            return;
        }

        uint64_t const process_time = file_time_value(process_kernel) + file_time_value(process_user);
        uint64_t const system_time = file_time_value(system_kernel) + file_time_value(system_user);
        if (m_last_process_time_100ns != 0u && system_time > m_last_system_time_100ns)
        {
            double const usage = std::clamp(
                100.0 * static_cast<double>(process_time - m_last_process_time_100ns) /
                static_cast<double>(system_time - m_last_system_time_100ns),
                0.0,
                100.0);
            wchar_t text[24]{};
            swprintf_s(text, L"CPU %.1f%%", usage);
            StatusCpuText().Text(text);
        }
        m_last_process_time_100ns = process_time;
        m_last_system_time_100ns = system_time;
    }

    void MainWindow::ApplyRfSnapshot(dab_receive_snapshot_t const& snapshot)
    {
        m_latest_rf_snapshot = snapshot;
        m_have_latest_rf_snapshot = true;
        UpdateRfContext();
        m_rf_null_locked = snapshot.locked != 0;
        if (m_rf_null_controls_ready)
        {
            RfNullLockText().Text(m_rf_null_locked ? L"LOCKED" : L"ACQUIRING");
            RfNullLockIndicator().Fill(Application::Current().Resources().Lookup(
                box_value(hstring{ m_rf_null_locked
                    ? L"MeasurementAccentBrush"
                    : L"WarningBrush" })).as<Brush>());
        }
        RfLiveBadgeText().Text(L"● LIVE");

        RfPowerValueText().Text(snapshot.calibrated_power_valid != 0
            ? format_float(snapshot.calibrated_power_dbm, L" dBm")
            : snapshot.adc_level_valid
                ? format_float(snapshot.adc_level_dbfs, L" dBFS")
                : hstring{ L"—" });
        RfSummaryPowerText().Text(snapshot.calibrated_power_valid != 0
            ? format_float(snapshot.calibrated_power_dbm, L" dBm")
            : snapshot.adc_level_valid
                ? format_float(snapshot.adc_level_dbfs, L" dBFS")
                : hstring{ L"—" });
        RfPowerDetailText().Text(snapshot.calibrated_power_valid != 0
            ? hstring{ L"Connector-referenced · " } +
                to_hstring(snapshot.calibration_profile_id) +
                L" · ±" +
                format_float(snapshot.calibrated_power_uncertainty_db,
                             L" dB",
                             2)
            : hstring{ L"ADC-referenced · " } +
                to_hstring(dab_rf_calibration_status_text(
                    snapshot.calibration_status)));
        RfAdcValueText().Text(snapshot.adc_level_valid
            ? format_float(snapshot.adc_level_dbfs, L" dBFS")
            : hstring{ L"— dBFS" });
        RfCnValueText().Text(snapshot.cn_valid
            ? format_float(snapshot.cn_db, L" dB")
            : hstring{ L"— dB" });
        RfCfoValueText().Text(snapshot.cfo_valid
            ? format_float(snapshot.cfo_hz, L" Hz")
            : hstring{ L"— Hz" });
        RfResidualCfoText().Text(snapshot.cfo_valid
            ? hstring{ L"Residual " } + format_float(snapshot.residual_cfo_hz, L" Hz")
            : hstring{ L"Residual — Hz" });
        RfOverrunValueText().Text(to_hstring(snapshot.dropped_blocks));
        RfConfigSampleRateText().Text(snapshot.sample_rate_hz != 0u
            ? format_float(static_cast<float>(snapshot.sample_rate_hz) / 1000000.0f, L" MS/s", 3)
            : hstring{ L"— MS/s" });

        bool const have_rf_input = snapshot.adc_level_valid != 0 &&
                                   snapshot.adc_window_sample_count != 0u;
        RfInputRmsText().Text(snapshot.adc_level_valid
            ? format_float(snapshot.adc_level_dbfs, L" dBFS")
            : hstring{ L"— dBFS" });
        RfInputPeakText().Text(have_rf_input
            ? format_float(snapshot.adc_peak_dbfs, L" dBFS")
            : hstring{ L"— dBFS" });
        RfInputCrestText().Text(have_rf_input
            ? format_float(snapshot.adc_crest_factor_db, L" dB")
            : hstring{ L"— dB" });
        RfInputCalibratedPowerText().Text(snapshot.calibrated_power_valid != 0
            ? format_float(snapshot.calibrated_power_dbm, L" dBm")
            : hstring{ L"Not calibrated" });
        if (have_rf_input)
        {
            wchar_t clipping_text[80]{};
            swprintf_s(clipping_text,
                       L"%.3f%%  ·  %u sample%s",
                       static_cast<double>(snapshot.adc_clipping_percent),
                       snapshot.adc_clipped_sample_count,
                       snapshot.adc_clipped_sample_count == 1u ? L"" : L"s");
            RfInputClippingText().Text(clipping_text);
            RfInputWindowText().Text(to_hstring(snapshot.adc_window_sample_count) + L" samples");
            RfClippingText().Text(hstring{ L"Clipping " } + clipping_text);
        }
        else
        {
            RfInputClippingText().Text(L"—");
            RfInputWindowText().Text(L"— samples");
            RfClippingText().Text(L"Awaiting ADC utilisation measurement");
        }

        RfInputIDcText().Text(have_rf_input
            ? format_signed_float(snapshot.adc_i_dc, L" FS", 5)
            : hstring{ L"— FS" });
        RfInputQDcText().Text(have_rf_input
            ? format_signed_float(snapshot.adc_q_dc, L" FS", 5)
            : hstring{ L"— FS" });
        bool const have_iq_quality = have_rf_input && snapshot.adc_iq_quality_valid != 0;
        RfInputAmplitudeText().Text(have_iq_quality
            ? format_signed_float(snapshot.adc_iq_amplitude_imbalance_db, L" dB", 2)
            : hstring{ L"— dB" });
        RfInputPhaseText().Text(have_iq_quality
            ? format_signed_float(snapshot.adc_iq_phase_imbalance_deg, L"°", 2)
            : hstring{ L"—°" });
        RfInputCorrelationText().Text(have_iq_quality
            ? format_signed_float(snapshot.adc_iq_correlation, L"", 4)
            : hstring{ L"—" });

        RfInputGainText().Text(snapshot.applied_system_gain_valid != 0
            ? format_float(snapshot.applied_system_gain_db, L" dB", 1)
            : snapshot.applied_gain_valid != 0
                ? format_integer(snapshot.if_gain_reduction_db,
                                 L" dB GR")
                : hstring{ L"Not reported" });
        RfInputLnaText().Text(snapshot.lna_state_valid != 0
            ? hstring{ L"State " } + to_hstring(snapshot.lna_state)
            : hstring{ L"Not reported" });
        RfInputIfGainText().Text(snapshot.if_gain_reduction_valid != 0
            ? format_integer(snapshot.if_gain_reduction_db, L" dB")
            : hstring{ L"Not reported" });
        RfInputAgcText().Text(snapshot.agc_enabled != 0 ? L"Enabled" : L"Disabled");
        RfInputTemperatureText().Text(snapshot.device_temperature_valid != 0
            ? format_float(snapshot.device_temperature_c, L" °C")
            : hstring{ L"Not reported" });

        RfInputReceivedBlocksText().Text(to_hstring(snapshot.blocks_received));
        RfInputDroppedBlocksText().Text(to_hstring(snapshot.dropped_blocks));
        if (snapshot.input_queue_capacity != 0u)
        {
            double const current_percent = 100.0 * snapshot.input_queue_depth /
                                           snapshot.input_queue_capacity;
            double const maximum_percent = 100.0 * snapshot.input_queue_high_watermark /
                                           snapshot.input_queue_capacity;
            wchar_t queue_text[64]{};
            swprintf_s(queue_text,
                       L"%.0f%% current  ·  %.0f%% max",
                       current_percent,
                       maximum_percent);
            RfInputQueueText().Text(queue_text);
        }
        else
        {
            RfInputQueueText().Text(L"Not reported");
        }
        RfInputDataRateText().Text(snapshot.sample_rate_hz != 0u
            ? format_float(static_cast<float>(snapshot.sample_rate_hz) * 4.0f / 1000000.0f,
                           L" MB/s",
                           2)
            : hstring{ L"— MB/s" });
        RfInputHardwareOverrunsText().Text(snapshot.hardware_overruns_valid != 0
            ? to_hstring(snapshot.hardware_overruns)
            : hstring{ L"Not reported" });
        RfInputProcessingLateText().Text(to_hstring(snapshot.processing_late_count));
        RfInputLatencyText().Text(snapshot.processing_latency_ns != 0u
            ? format_float(static_cast<float>(snapshot.processing_latency_ns) / 1000000.0f,
                           L" ms",
                           2)
            : hstring{ L"— ms" });
        RfInputCallbackText().Text(snapshot.input_block_interval_ns != 0u
            ? format_float(static_cast<float>(snapshot.input_block_interval_ns) / 1000000.0f,
                           L" ms",
                           2)
            : hstring{ L"— ms" });
        UpdateRfInputHistogram(snapshot);
        ApplyRfSyncSnapshot(snapshot);

        if (have_rf_input && snapshot.adc_clipped_sample_count != 0u)
        {
            RfWarningsText().Text(L"ADC clipping detected in the latest PRS analysis window.");
        }
        else if (snapshot.calibrated_power_valid != 0)
        {
            RfWarningsText().Text(L"No current RF warnings.");
        }
        else
        {
            RfWarningsText().Text(
                hstring{ L"Power remains ADC-referenced: " } +
                to_hstring(dab_rf_calibration_status_text(
                    snapshot.calibration_status)) +
                L".");
        }

        uint64_t const timestamp_ns = snapshot.timestamp_ns != 0u
            ? snapshot.timestamp_ns
            : m_tick * 250000000ull;
        if (snapshot.locked != 0)
        {
            if (!m_rf_was_locked || timestamp_ns < m_rf_lock_started_ns)
            {
                m_rf_lock_started_ns = timestamp_ns;
            }
            m_rf_was_locked = true;
            uint64_t const stable_seconds = (timestamp_ns - m_rf_lock_started_ns) / 1000000000ull;
            wchar_t stable_text[48]{};
            swprintf_s(stable_text,
                       L"Stable %02llu:%02llu:%02llu",
                       static_cast<unsigned long long>(stable_seconds / 3600ull),
                       static_cast<unsigned long long>((stable_seconds / 60ull) % 60ull),
                       static_cast<unsigned long long>(stable_seconds % 60ull));
            RfStatusValueText().Text(L"LOCKED");
            RfStatusDetailText().Text(stable_text);
            RfLockIndicator().Fill(
                Application::Current().Resources().Lookup(box_value(hstring{ L"MeasurementAccentBrush" })).as<Brush>());
        }
        else
        {
            m_rf_was_locked = false;
            m_rf_lock_started_ns = 0u;
            RfStatusValueText().Text(L"ACQUIRING");
            RfStatusDetailText().Text(L"Waiting for stable synchronization");
            RfLockIndicator().Fill(
                Application::Current().Resources().Lookup(box_value(hstring{ L"WarningBrush" })).as<Brush>());
        }

        float const missing = (std::numeric_limits<float>::quiet_NaN)();
        AppendRfHistory(snapshot.adc_level_valid ? snapshot.adc_level_dbfs : missing,
                        snapshot.cn_valid ? snapshot.cn_db : missing);
        AppendRfLongHistory(snapshot);
        EvaluateRfAlarm(snapshot);
        UpdateCalibrationUi(&snapshot);
    }

    void MainWindow::UpdateRfInputHistogram(dab_receive_snapshot_t const& snapshot)
    {
        uint32_t maximum = 0u;
        for (size_t bin = 0u; bin < DAB_RECEIVE_ADC_HISTOGRAM_BIN_COUNT; ++bin)
        {
            maximum = (std::max)(maximum, snapshot.adc_i_histogram[bin]);
            maximum = (std::max)(maximum, snapshot.adc_q_histogram[bin]);
        }
        RfInputIHistogramTrace().Points(histogram_points(snapshot.adc_i_histogram,
                                                         maximum,
                                                         720.0f,
                                                         220.0f));
        RfInputQHistogramTrace().Points(histogram_points(snapshot.adc_q_histogram,
                                                         maximum,
                                                         720.0f,
                                                         220.0f));
    }

    void MainWindow::ApplyRfSyncSnapshot(dab_receive_snapshot_t const& snapshot)
    {
        AppendRfSyncHistory(snapshot);

        bool const locked = snapshot.locked != 0;
        RfSyncStateText().Text(locked ? L"FRAME LOCKED" : L"ACQUIRING");
        RfSyncTrackingText().Text(locked ? L"Frame tracking" : L"PRS acquisition");
        RfSyncStateIndicator().Fill(Application::Current().Resources().Lookup(
            box_value(hstring{ locked ? L"MeasurementAccentBrush" : L"WarningBrush" })).as<Brush>());
        RfSyncLockDurationText().Text(locked
            ? format_duration(snapshot.sync_lock_duration_ns)
            : hstring{ L"00:00:00" });
        RfSyncReacquisitionText().Text(to_hstring(snapshot.sync_reacquisition_count));
        RfSyncLossCountText().Text(to_hstring(snapshot.sync_loss_count));

        RfSyncCoarseCfoText().Text(snapshot.cfo_valid
            ? format_signed_float(snapshot.cfo_hz, L" Hz", 1)
            : hstring{ L"— Hz" });
        RfSyncFineCfoText().Text(snapshot.cfo_valid
            ? format_signed_float(snapshot.fine_cfo_hz, L" Hz", 1)
            : hstring{ L"— Hz" });
        RfSyncResidualCfoText().Text(snapshot.cfo_valid
            ? format_signed_float(snapshot.residual_cfo_hz, L" Hz", 1)
            : hstring{ L"— Hz" });
        RfSyncCarrierOffsetText().Text(snapshot.cfo_valid
            ? format_signed_float(static_cast<float>(snapshot.carrier_offset_bins), L" bins", 0)
            : hstring{ L"— bins" });

        RfSyncClockErrorText().Text(snapshot.sample_clock_error_valid
            ? format_signed_float(snapshot.sample_clock_error_ppm, L" ppm", 3)
            : hstring{ L"Estimating…" });
        RfSyncClockCorrectionText().Text(snapshot.sample_clock_error_valid
            ? format_signed_float(-snapshot.sample_clock_error_ppm, L" ppm", 3)
            : hstring{ L"— ppm" });
        RfSyncClockFramesText().Text(to_hstring(snapshot.sample_clock_frame_count));
        RfSampleClockValueText().Text(snapshot.sample_clock_error_valid
            ? format_signed_float(snapshot.sample_clock_error_ppm, L" ppm", 3)
            : hstring{ L"— ppm" });
        RfSampleClockDetailText().Text(snapshot.sample_clock_error_valid
            ? hstring{ L"Suggested correction " } +
              format_signed_float(-snapshot.sample_clock_error_ppm, L" ppm", 3)
            : hstring{ L"Estimating from frame period" });

        RfSyncNullConfidenceText().Text(snapshot.null_confidence_valid
            ? format_float(snapshot.null_confidence, L"", 3)
            : hstring{ L"—" });
        RfSyncCpCorrelationText().Text(snapshot.cp_correlation_valid
            ? format_float(snapshot.cp_correlation_peak, L"", 3)
            : hstring{ L"—" });
        RfSyncPrsCorrelationText().Text(snapshot.prs_valid
            ? format_float(snapshot.prs_score, L"", 3)
            : hstring{ L"—" });
        RfSyncFftTimingText().Text(snapshot.fft_timing_valid
            ? format_signed_float(snapshot.fft_timing_offset_samples, L" samples", 2)
            : hstring{ L"— samples" });
        RfSyncFrameTimingText().Text(snapshot.timing_valid
            ? hstring{ L"Frame error " } + format_integer(snapshot.timing_error_samples, L" samples")
            : hstring{ L"Frame error — samples" });
        RfSyncAcquisitionText().Text(snapshot.sync_acquisition_time_valid
            ? format_float(snapshot.sync_acquisition_time_ms, L" ms", 1)
            : hstring{ L"In progress" });
        RfSyncFrameCountsText().Text(to_hstring(snapshot.good_frames) + L" / " +
                                     to_hstring(snapshot.bad_frames));

        if (snapshot.sync_loss_count > m_rf_sync_last_loss_count)
        {
            hstring const time = local_clock_text();
            RfSyncLastLossText().Text(time);
            AddRfSyncEvent(L"Frame lock lost");
            AddRfHistoryEvent(L"Warning",
                              L"Synchronisation",
                              L"Lost",
                              L"Frame lock lost; reacquisition in progress",
                              snapshot.timestamp_ns);
        }
        if (snapshot.sync_reacquisition_count > m_rf_sync_last_reacquisition_count)
        {
            AddRfSyncEvent(hstring{ L"Frame lock reacquired after " } +
                           format_float(snapshot.sync_acquisition_time_ms, L" ms", 1));
            AddRfHistoryEvent(L"Info",
                              L"Synchronisation",
                              L"Recovered",
                              hstring{ L"Reacquired in " } +
                                  format_float(snapshot.sync_acquisition_time_ms, L" ms", 1),
                              snapshot.timestamp_ns);
        }
        if (locked && !m_rf_sync_seen_lock)
        {
            m_rf_sync_seen_lock = true;
            AddRfSyncEvent(hstring{ L"Initial frame lock acquired after " } +
                           format_float(snapshot.sync_acquisition_time_ms, L" ms", 1));
            AddRfHistoryEvent(L"Info",
                              L"Synchronisation",
                              L"Locked",
                              hstring{ L"Initial lock in " } +
                                  format_float(snapshot.sync_acquisition_time_ms, L" ms", 1),
                              snapshot.timestamp_ns);
        }
        m_rf_sync_last_loss_count = snapshot.sync_loss_count;
        m_rf_sync_last_reacquisition_count = snapshot.sync_reacquisition_count;
    }

    void MainWindow::AppendRfSyncHistory(dab_receive_snapshot_t const& snapshot)
    {
        float const missing = (std::numeric_limits<float>::quiet_NaN)();
        m_rf_sync_coarse_history[m_rf_sync_history_write] = snapshot.cfo_valid
            ? snapshot.cfo_hz : missing;
        m_rf_sync_fine_history[m_rf_sync_history_write] = snapshot.cfo_valid
            ? snapshot.fine_cfo_hz : missing;
        m_rf_sync_residual_history[m_rf_sync_history_write] = snapshot.cfo_valid
            ? snapshot.residual_cfo_hz : missing;
        m_rf_sync_clock_history[m_rf_sync_history_write] = snapshot.sample_clock_error_valid
            ? snapshot.sample_clock_error_ppm : missing;
        m_rf_sync_timestamp_history[m_rf_sync_history_write] = snapshot.timestamp_ns != 0u
            ? snapshot.timestamp_ns
            : m_tick * 250000000ull;
        m_rf_sync_history_write = (m_rf_sync_history_write + 1u) % rf_sync_history_capacity;
        m_rf_sync_history_count = (std::min)(m_rf_sync_history_count + 1u,
                                             rf_sync_history_capacity);
        UpdateRfSyncHistoryVisuals();
    }

    void MainWindow::UpdateRfSyncHistoryVisuals()
    {
        float maximum = 0.0f;
        size_t const first = (m_rf_sync_history_write + rf_sync_history_capacity -
                              m_rf_sync_history_count) % rf_sync_history_capacity;
        for (size_t point = 0u; point < m_rf_sync_history_count; ++point)
        {
            size_t const index = (first + point) % rf_sync_history_capacity;
            if (std::isfinite(m_rf_sync_coarse_history[index]))
            {
                maximum = (std::max)(maximum, std::fabs(m_rf_sync_coarse_history[index]));
            }
            if (std::isfinite(m_rf_sync_fine_history[index]))
            {
                maximum = (std::max)(maximum, std::fabs(m_rf_sync_fine_history[index]));
            }
            if (std::isfinite(m_rf_sync_residual_history[index]))
            {
                maximum = (std::max)(maximum, std::fabs(m_rf_sync_residual_history[index]));
            }
        }
        float const range = (std::max)(50.0f,
            std::ceil((maximum * 1.10f) / 50.0f) * 50.0f);
        RfSyncCoarseHistoryTrace().Points(history_points(m_rf_sync_coarse_history,
                                                         m_rf_sync_history_write,
                                                         m_rf_sync_history_count,
                                                         -range,
                                                         range,
                                                         900.0f,
                                                         220.0f));
        RfSyncFineHistoryTrace().Points(history_points(m_rf_sync_fine_history,
                                                       m_rf_sync_history_write,
                                                       m_rf_sync_history_count,
                                                       -range,
                                                       range,
                                                       900.0f,
                                                       220.0f));
        RfSyncResidualHistoryTrace().Points(history_points(m_rf_sync_residual_history,
                                                           m_rf_sync_history_write,
                                                           m_rf_sync_history_count,
                                                           -range,
                                                           range,
                                                           900.0f,
                                                           220.0f));
        RfSyncCfoAxisTopText().Text(format_signed_float(range, L" Hz", 0));
        RfSyncCfoAxisBottomText().Text(format_signed_float(-range, L" Hz", 0));

        float drift = 0.0f;
        RfSyncCfoDriftText().Text(timed_history_slope(m_rf_sync_coarse_history,
                                                      m_rf_sync_timestamp_history,
                                                      m_rf_sync_history_write,
                                                      m_rf_sync_history_count,
                                                      drift)
            ? format_signed_float(drift, L" Hz/min", 2)
            : hstring{ L"— Hz/min" });
        RfSyncClockDriftText().Text(timed_history_slope(m_rf_sync_clock_history,
                                                        m_rf_sync_timestamp_history,
                                                        m_rf_sync_history_write,
                                                        m_rf_sync_history_count,
                                                        drift)
            ? format_signed_float(drift, L" ppm/min", 3)
            : hstring{ L"— ppm/min" });
    }

    void MainWindow::ResetRfSyncDiagnostics()
    {
        float const missing = (std::numeric_limits<float>::quiet_NaN)();
        m_rf_sync_coarse_history.fill(missing);
        m_rf_sync_fine_history.fill(missing);
        m_rf_sync_residual_history.fill(missing);
        m_rf_sync_clock_history.fill(missing);
        m_rf_sync_timestamp_history.fill(0u);
        m_rf_sync_history_write = 0u;
        m_rf_sync_history_count = 0u;
        m_rf_sync_last_loss_count = 0u;
        m_rf_sync_last_reacquisition_count = 0u;
        m_rf_sync_seen_lock = false;
        RfSyncStateText().Text(L"ACQUIRING");
        RfSyncTrackingText().Text(L"Opening receiver");
        RfSyncStateIndicator().Fill(Application::Current().Resources().Lookup(
            box_value(hstring{ L"WarningBrush" })).as<Brush>());
        RfSyncLockDurationText().Text(L"00:00:00");
        RfSyncReacquisitionText().Text(L"0");
        RfSyncLossCountText().Text(L"0");
        RfSyncLastLossText().Text(L"None this session");
        RfSyncEventList().Items().Clear();
        AddRfSyncEvent(L"Receiver started — acquiring frame lock");
        UpdateRfSyncHistoryVisuals();
    }

    void MainWindow::AddRfSyncEvent(hstring const& message)
    {
        auto items = RfSyncEventList().Items();
        items.Append(box_value(local_clock_text() + L"  ·  " + message));
        while (items.Size() > 10u)
        {
            items.RemoveAt(0u);
        }
        RfSyncEventList().ScrollIntoView(items.GetAt(items.Size() - 1u));
    }

    void MainWindow::AppendRfHistory(float power_dbfs, float cn_db)
    {
        m_rf_power_history[m_rf_history_write] = power_dbfs;
        m_rf_cn_history[m_rf_history_write] = cn_db;
        m_rf_mer_history[m_rf_history_write] = m_latest_mer_valid
            ? m_latest_mer_db
            : (std::numeric_limits<float>::quiet_NaN)();
        m_rf_history_write = (m_rf_history_write + 1u) % rf_history_capacity;
        m_rf_history_count = (std::min)(m_rf_history_count + 1u, rf_history_capacity);
        UpdateRfHistoryVisuals();
    }

    void MainWindow::UpdateRfHistoryVisuals()
    {
        RfPowerHistoryTrace().Points(history_points(m_rf_power_history,
                                                    m_rf_history_write,
                                                    m_rf_history_count,
                                                    -90.0f,
                                                    0.0f,
                                                    520.0f,
                                                    45.0f));
        RfCnHistoryTrace().Points(history_points(m_rf_cn_history,
                                                 m_rf_history_write,
                                                 m_rf_history_count,
                                                 0.0f,
                                                 40.0f,
                                                 520.0f,
                                                 45.0f));
        RfMerHistoryTrace().Points(history_points(m_rf_mer_history,
                                                  m_rf_history_write,
                                                  m_rf_history_count,
                                                  0.0f,
                                                  40.0f,
                                                  520.0f,
                                                  45.0f));
        RfPowerSparkline().Points(history_points(m_rf_power_history,
                                                 m_rf_history_write,
                                                 m_rf_history_count,
                                                 -90.0f,
                                                 0.0f,
                                                 100.0f,
                                                 24.0f));
        RfCnSparkline().Points(history_points(m_rf_cn_history,
                                              m_rf_history_write,
                                              m_rf_history_count,
                                              0.0f,
                                              40.0f,
                                              100.0f,
                                              24.0f));
        RfMerSparkline().Points(history_points(m_rf_mer_history,
                                               m_rf_history_write,
                                               m_rf_history_count,
                                               0.0f,
                                               40.0f,
                                               100.0f,
                                               24.0f));
    }

    void MainWindow::RfHistory_Loaded(IInspectable const&, RoutedEventArgs const&)
    {
        if (m_rf_history_controls_ready)
        {
            return;
        }
        m_rf_history_controls_ready = true;
        ApplyRfAlarmConfiguration(false);
        RefreshRfHistoryEventList();
        UpdateRfLongHistoryVisuals();
    }

    void MainWindow::RfHistoryPeriod_SelectionChanged(IInspectable const&,
                                                       SelectionChangedEventArgs const&)
    {
        if (m_rf_history_controls_ready)
        {
            UpdateRfLongHistoryVisuals();
        }
    }

    void MainWindow::RfHistoryControls_Changed(IInspectable const&, RoutedEventArgs const&)
    {
        if (m_rf_history_controls_ready)
        {
            UpdateRfLongHistoryVisuals();
        }
    }

    void MainWindow::RfHistoryPause_Click(IInspectable const&, RoutedEventArgs const&)
    {
        m_rf_history_display_paused = toggle_is_checked(RfHistoryPauseButton());
        if (m_rf_history_display_paused && m_rf_long_history_count != 0u)
        {
            size_t const latest = (m_rf_long_history_write + rf_long_history_capacity - 1u) %
                                  rf_long_history_capacity;
            m_rf_history_pause_timestamp_ns = m_rf_long_history[latest].timestamp_ns;
        }
        else
        {
            m_rf_history_pause_timestamp_ns = 0u;
        }
        RfHistoryPauseButton().Content(box_value(m_rf_history_display_paused
            ? hstring{ L"Resume display" }
            : hstring{ L"Pause display" }));
        RfHistoryCaptureStatusText().Text(m_rf_history_display_paused
            ? L"Display paused · capture and alarms remain live"
            : L"Collecting live samples");
        UpdateRfLongHistoryVisuals();
    }

    uint64_t MainWindow::RfHistoryPeriodNs()
    {
        auto const selected = RfHistoryPeriodComboBox().SelectedItem().try_as<ComboBoxItem>();
        if (!selected || !selected.Tag())
        {
            return 600000000000ull;
        }
        auto const tag = unbox_value<hstring>(selected.Tag());
        wchar_t* end = nullptr;
        uint64_t const seconds = std::wcstoull(tag.c_str(), &end, 10);
        return end != tag.c_str() ? seconds * 1000000000ull : 600000000000ull;
    }

    void MainWindow::AppendRfLongHistory(dab_receive_snapshot_t const& snapshot)
    {
        RfHistorySample sample{};
        sample.timestamp_ns = snapshot.timestamp_ns != 0u
            ? snapshot.timestamp_ns
            : m_tick * 250000000ull;
        sample.wall_time_100ns = current_wall_time_100ns();
        sample.power_dbfs = snapshot.adc_level_dbfs;
        sample.mer_db = m_latest_mer_db;
        sample.cfo_hz = snapshot.cfo_hz;
        sample.clipping_percent = snapshot.adc_clipping_percent;
        sample.power_valid = snapshot.adc_level_valid != 0;
        sample.mer_valid = snapshot.locked != 0 && m_latest_mer_valid;
        sample.cfo_valid = snapshot.cfo_valid != 0;
        sample.clipping_valid = snapshot.adc_window_sample_count != 0u;
        sample.locked = snapshot.locked != 0;
        m_rf_long_history[m_rf_long_history_write] = sample;
        m_rf_long_history_write = (m_rf_long_history_write + 1u) % rf_long_history_capacity;
        m_rf_long_history_count = (std::min)(m_rf_long_history_count + 1u,
                                              rf_long_history_capacity);
        if (!m_rf_history_display_paused)
        {
            UpdateRfLongHistoryVisuals();
        }
    }

    void MainWindow::UpdateRfLongHistoryVisuals()
    {
        if (!m_rf_history_controls_ready)
        {
            return;
        }

        RfLongHistoryPowerTrace().Visibility(toggle_is_checked(RfHistoryPowerButton())
            ? Visibility::Visible : Visibility::Collapsed);
        RfLongHistoryMerTrace().Visibility(toggle_is_checked(RfHistoryMerButton())
            ? Visibility::Visible : Visibility::Collapsed);
        RfLongHistoryCfoTrace().Visibility(toggle_is_checked(RfHistoryCfoButton())
            ? Visibility::Visible : Visibility::Collapsed);
        RfLongHistoryClippingTrace().Visibility(toggle_is_checked(RfHistoryClippingButton())
            ? Visibility::Visible : Visibility::Collapsed);

        if (m_rf_long_history_count == 0u)
        {
            RfHistoryWindowStatusText().Text(L"Awaiting receiver samples");
            RfHistoryEventMarkerCanvas().Children().Clear();
            return;
        }

        size_t const oldest = (m_rf_long_history_write + rf_long_history_capacity -
                               m_rf_long_history_count) % rf_long_history_capacity;
        size_t const newest = (m_rf_long_history_write + rf_long_history_capacity - 1u) %
                              rf_long_history_capacity;
        uint64_t const end_ns = m_rf_history_display_paused &&
            m_rf_history_pause_timestamp_ns != 0u
            ? m_rf_history_pause_timestamp_ns
            : m_rf_long_history[newest].timestamp_ns;
        uint64_t const period_ns = RfHistoryPeriodNs();
        uint64_t start_ns = period_ns != 0u && end_ns > period_ns
            ? end_ns - period_ns
            : m_rf_long_history[oldest].timestamp_ns;
        if (start_ns > end_ns)
        {
            start_ns = end_ns;
        }
        uint64_t const span_ns = (std::max)(end_ns - start_ns, 1ull);

        float cfo_range = 50.0f;
        float clipping_range = 1.0f;
        size_t visible_samples = 0u;
        for (size_t offset = 0u; offset < m_rf_long_history_count; ++offset)
        {
            auto const& sample = m_rf_long_history[(oldest + offset) % rf_long_history_capacity];
            if (sample.timestamp_ns < start_ns || sample.timestamp_ns > end_ns)
            {
                continue;
            }
            ++visible_samples;
            if (sample.cfo_valid && std::isfinite(sample.cfo_hz))
            {
                cfo_range = (std::max)(cfo_range, std::fabs(sample.cfo_hz) * 1.10f);
            }
            if (sample.clipping_valid && std::isfinite(sample.clipping_percent))
            {
                clipping_range = (std::max)(clipping_range, sample.clipping_percent * 1.15f);
            }
        }
        cfo_range = std::ceil(cfo_range / 50.0f) * 50.0f;
        clipping_range = std::ceil(clipping_range * 10.0f) / 10.0f;

        auto build_points = [&](uint32_t metric,
                                float minimum,
                                float maximum,
                                float lane_top) -> PointCollection
        {
            PointCollection points;
            for (size_t offset = 0u; offset < m_rf_long_history_count; ++offset)
            {
                auto const& sample = m_rf_long_history[(oldest + offset) % rf_long_history_capacity];
                if (sample.timestamp_ns < start_ns || sample.timestamp_ns > end_ns)
                {
                    continue;
                }
                bool valid = false;
                float value = 0.0f;
                switch (metric)
                {
                case 0u:
                    valid = sample.power_valid;
                    value = sample.power_dbfs;
                    break;
                case 1u:
                    valid = sample.mer_valid;
                    value = sample.mer_db;
                    break;
                case 2u:
                    valid = sample.cfo_valid;
                    value = sample.cfo_hz;
                    break;
                default:
                    valid = sample.clipping_valid;
                    value = sample.clipping_percent;
                    break;
                }
                if (!valid || !std::isfinite(value))
                {
                    continue;
                }
                float const x = 90.0f + 890.0f * static_cast<float>(
                    static_cast<double>(sample.timestamp_ns - start_ns) /
                    static_cast<double>(span_ns));
                float const normalized = std::clamp((value - minimum) /
                                                     (maximum - minimum),
                                                     0.0f,
                                                     1.0f);
                float const y = lane_top + (1.0f - normalized) * 55.0f;
                points.Append(Point{ x, y });
            }
            return points;
        };

        RfLongHistoryPowerTrace().Points(build_points(0u, -100.0f, 0.0f, 10.0f));
        RfLongHistoryMerTrace().Points(build_points(1u, 0.0f, 40.0f, 75.0f));
        RfLongHistoryCfoTrace().Points(build_points(2u, -cfo_range, cfo_range, 140.0f));
        RfLongHistoryClippingTrace().Points(build_points(3u, 0.0f, clipping_range, 205.0f));
        RfHistoryCfoAxisText().Text(L"±" + format_float(cfo_range, L" Hz", 0));
        RfHistoryClippingAxisText().Text(L"0…" + format_float(clipping_range, L" %", 1));

        auto markers = RfHistoryEventMarkerCanvas().Children();
        markers.Clear();
        auto const warning_brush = SolidColorBrush(ColorHelper::FromArgb(220u, 255u, 200u, 87u));
        auto const annotation_brush = SolidColorBrush(ColorHelper::FromArgb(220u, 84u, 226u, 255u));
        for (auto const& event : m_rf_history_events)
        {
            if (event.timestamp_ns < start_ns || event.timestamp_ns > end_ns ||
                (event.severity != L"Warning" && event.severity != L"Annotation"))
            {
                continue;
            }
            double const x = 90.0 + 890.0 * static_cast<double>(event.timestamp_ns - start_ns) /
                static_cast<double>(span_ns);
            auto const brush = event.severity == L"Warning" ? warning_brush : annotation_brush;
            Line line;
            line.X1(x);
            line.X2(x);
            line.Y1(10.0);
            line.Y2(260.0);
            line.Stroke(brush);
            line.StrokeThickness(1.0);
            line.Opacity(0.45);
            markers.Append(line);
            PointCollection triangle_points;
            triangle_points.Append(Point{ static_cast<float>(x - 5.0), 267.0f });
            triangle_points.Append(Point{ static_cast<float>(x + 5.0), 267.0f });
            triangle_points.Append(Point{ static_cast<float>(x), 259.0f });
            Microsoft::UI::Xaml::Shapes::Polygon triangle;
            triangle.Points(triangle_points);
            triangle.Fill(brush);
            markers.Append(triangle);
        }

        RfHistoryAxisLeftText().Text(relative_history_text(span_ns));
        RfHistoryAxisCenterText().Text(relative_history_text(span_ns / 2u));
        auto const selected = RfHistoryPeriodComboBox().SelectedItem().try_as<ComboBoxItem>();
        hstring const period_label = selected
            ? unbox_value<hstring>(selected.Content())
            : hstring{ L"Selected period" };
        RfHistoryWindowStatusText().Text(period_label + L" · " +
            to_hstring(visible_samples) + L" samples" +
            (m_rf_history_display_paused ? L" · paused" : L""));
    }

    void MainWindow::ResetRfHistorySession()
    {
        m_rf_long_history = {};
        m_rf_long_history_write = 0u;
        m_rf_long_history_count = 0u;
        m_rf_history_events.clear();
        m_rf_history_display_paused = false;
        m_rf_history_pause_timestamp_ns = 0u;
        if (RfHistoryPauseButton())
        {
            RfHistoryPauseButton().IsChecked(false);
            RfHistoryPauseButton().Content(box_value(L"Pause display"));
        }
        dab_measurement_alarm_reset(&m_rf_alarm);
        m_rf_alarm_last_value_valid = false;
        RfHistoryAlarmBanner().IsOpen(false);
        AddRfHistoryEvent(L"Info", L"Receiver", L"Started", L"New RF history session");
        ApplyRfAlarmConfiguration(false);
        UpdateRfLongHistoryVisuals();
    }

    void MainWindow::AddRfHistoryEvent(hstring const& severity,
                                       hstring const& measurement,
                                       hstring const& value,
                                       hstring const& description,
                                       uint64_t timestamp_ns)
    {
        RfHistoryEvent event{};
        if (timestamp_ns == 0u && m_rf_long_history_count != 0u)
        {
            size_t const newest = (m_rf_long_history_write + rf_long_history_capacity - 1u) %
                                  rf_long_history_capacity;
            timestamp_ns = m_rf_long_history[newest].timestamp_ns;
        }
        event.timestamp_ns = timestamp_ns != 0u ? timestamp_ns : m_tick * 250000000ull;
        event.wall_time_100ns = current_wall_time_100ns();
        event.severity = severity.c_str();
        event.measurement = measurement.c_str();
        event.value = value.c_str();
        event.description = description.c_str();
        m_rf_history_events.push_back(std::move(event));
        if (m_rf_history_events.size() > 250u)
        {
            m_rf_history_events.erase(m_rf_history_events.begin());
        }
        RefreshRfHistoryEventList();
        if (!m_rf_history_display_paused)
        {
            UpdateRfLongHistoryVisuals();
        }
    }

    void MainWindow::RefreshRfHistoryEventList()
    {
        if (!RfHistoryEventList())
        {
            return;
        }
        auto items = RfHistoryEventList().Items();
        items.Clear();
        for (auto iterator = m_rf_history_events.rbegin();
             iterator != m_rf_history_events.rend();
             ++iterator)
        {
            auto const& event = *iterator;
            hstring const time = history_clock_text(event.wall_time_100ns);
            wchar_t row_text[768]{};
            swprintf_s(row_text,
                       L"%-11ls %-11.11ls %-23.23ls %-16.16ls %ls",
                       time.c_str(),
                       event.severity.c_str(),
                       event.measurement.c_str(),
                       event.value.c_str(),
                       event.description.c_str());
            TextBlock row;
            row.Text(row_text);
            row.FontFamily(FontFamily{ L"Consolas" });
            row.FontSize(12.0);
            row.TextWrapping(TextWrapping::NoWrap);
            if (event.severity == L"Warning")
            {
                row.Foreground(Application::Current().Resources().Lookup(
                    box_value(hstring{ L"WarningBrush" })).as<Brush>());
            }
            else if (event.severity == L"Annotation")
            {
                row.Foreground(Application::Current().Resources().Lookup(
                    box_value(hstring{ L"SpectrumBrush" })).as<Brush>());
            }
            items.Append(row);
        }
    }

    bool MainWindow::ApplyRfAlarmConfiguration(bool add_event)
    {
        double const threshold = RfHistoryAlarmThresholdBox().Value();
        double const recovery = RfHistoryAlarmRecoveryBox().Value();
        double const duration_seconds = RfHistoryAlarmDurationBox().Value();
        int32_t const condition_index = RfHistoryAlarmConditionComboBox().SelectedIndex();
        if (!std::isfinite(threshold) || !std::isfinite(recovery) ||
            !std::isfinite(duration_seconds) || duration_seconds < 0.0 ||
            (condition_index == 0 && recovery <= threshold) ||
            (condition_index != 0 && recovery >= threshold))
        {
            RfHistoryAlarmStateText().Text(condition_index == 0
                ? L"Configuration error: recovery must be above the trigger threshold"
                : L"Configuration error: recovery must be below the trigger threshold");
            RfHistoryAlarmStateText().Foreground(Application::Current().Resources().Lookup(
                box_value(hstring{ L"WarningBrush" })).as<Brush>());
            return false;
        }

        dab_measurement_alarm_config_t config{};
        config.condition = condition_index == 0
            ? DAB_MEASUREMENT_ALARM_LESS_THAN
            : DAB_MEASUREMENT_ALARM_GREATER_THAN;
        config.threshold = static_cast<float>(threshold);
        config.recovery_threshold = static_cast<float>(recovery);
        config.minimum_duration_ns = static_cast<uint64_t>(
            std::llround(duration_seconds * 1000000000.0));
        dab_measurement_alarm_configure(&m_rf_alarm, &config);
        m_rf_alarm_measurement_index = RfHistoryAlarmMeasurementComboBox().SelectedIndex();
        m_rf_alarm_enabled = toggle_is_checked(RfHistoryAlarmEnableButton());
        m_rf_alarm_last_value_valid = false;
        RfHistoryAlarmBanner().IsOpen(false);

        std::array<wchar_t const*, 4> const names{ L"MER", L"Power", L"CFO magnitude", L"Clipping" };
        std::array<wchar_t const*, 4> const units{ L"dB", L"dBFS", L"Hz", L"%" };
        size_t const index = static_cast<size_t>(std::clamp(m_rf_alarm_measurement_index, 0, 3));
        wchar_t state_text[180]{};
        swprintf_s(state_text,
                   L"%ls: %ls %ls %.2f %ls for %.1f s · recovery %ls %.2f %ls",
                   m_rf_alarm_enabled ? L"Armed" : L"Disabled",
                   names[index],
                   condition_index == 0 ? L"<" : L">",
                   threshold,
                   units[index],
                   duration_seconds,
                   condition_index == 0 ? L"≥" : L"≤",
                   recovery,
                   units[index]);
        RfHistoryAlarmStateText().Text(state_text);
        RfHistoryAlarmStateText().Foreground(Application::Current().Resources().Lookup(
            box_value(hstring{ m_rf_alarm_enabled ? L"MeasurementAccentBrush" : L"MutedTextBrush" })).as<Brush>());
        RfHistoryAlarmEnableButton().Content(box_value(m_rf_alarm_enabled
            ? hstring{ L"Alarm enabled" }
            : hstring{ L"Alarm disabled" }));
        if (add_event)
        {
            AddRfHistoryEvent(L"Info",
                              L"Alarm configuration",
                              m_rf_alarm_enabled ? L"Enabled" : L"Disabled",
                              state_text);
        }
        return true;
    }

    void MainWindow::RfHistoryAlarmMeasurement_SelectionChanged(
        IInspectable const&,
        SelectionChangedEventArgs const&)
    {
        if (!m_rf_history_controls_ready)
        {
            return;
        }
        switch (RfHistoryAlarmMeasurementComboBox().SelectedIndex())
        {
        case 0:
            RfHistoryAlarmConditionComboBox().SelectedIndex(0);
            RfHistoryAlarmThresholdBox().Value(12.0);
            RfHistoryAlarmRecoveryBox().Value(14.0);
            break;
        case 1:
            RfHistoryAlarmConditionComboBox().SelectedIndex(0);
            RfHistoryAlarmThresholdBox().Value(-90.0);
            RfHistoryAlarmRecoveryBox().Value(-85.0);
            break;
        case 2:
            RfHistoryAlarmConditionComboBox().SelectedIndex(1);
            RfHistoryAlarmThresholdBox().Value(100.0);
            RfHistoryAlarmRecoveryBox().Value(50.0);
            break;
        default:
            RfHistoryAlarmConditionComboBox().SelectedIndex(1);
            RfHistoryAlarmThresholdBox().Value(0.10);
            RfHistoryAlarmRecoveryBox().Value(0.01);
            break;
        }
    }

    void MainWindow::RfHistoryAlarmApply_Click(IInspectable const&, RoutedEventArgs const&)
    {
        ApplyRfAlarmConfiguration(true);
    }

    void MainWindow::RfHistoryAlarmEnable_Click(IInspectable const&, RoutedEventArgs const&)
    {
        m_rf_alarm_enabled = toggle_is_checked(RfHistoryAlarmEnableButton());
        if (m_rf_alarm_enabled)
        {
            ApplyRfAlarmConfiguration(true);
        }
        else
        {
            dab_measurement_alarm_reset(&m_rf_alarm);
            RfHistoryAlarmBanner().IsOpen(false);
            RfHistoryAlarmEnableButton().Content(box_value(L"Alarm disabled"));
            RfHistoryAlarmStateText().Text(L"Disabled · history capture continues");
            RfHistoryAlarmStateText().Foreground(Application::Current().Resources().Lookup(
                box_value(hstring{ L"MutedTextBrush" })).as<Brush>());
            AddRfHistoryEvent(L"Info", L"Alarm configuration", L"Disabled",
                              L"Threshold evaluation stopped; history capture continues");
        }
    }

    void MainWindow::EvaluateRfAlarm(dab_receive_snapshot_t const& snapshot)
    {
        if (!m_rf_alarm_enabled || m_replay_loaded)
        {
            return;
        }
        bool valid = false;
        float value = 0.0f;
        std::array<wchar_t const*, 4> const names{ L"MER", L"Power", L"CFO magnitude", L"Clipping" };
        std::array<wchar_t const*, 4> const units{ L"dB", L"dBFS", L"Hz", L"%" };
        size_t const index = static_cast<size_t>(std::clamp(m_rf_alarm_measurement_index, 0, 3));
        switch (index)
        {
        case 0u:
            valid = snapshot.locked != 0 && m_latest_mer_valid;
            value = m_latest_mer_db;
            break;
        case 1u:
            valid = snapshot.adc_level_valid != 0;
            value = snapshot.adc_level_dbfs;
            break;
        case 2u:
            valid = snapshot.cfo_valid != 0;
            value = std::fabs(snapshot.cfo_hz);
            break;
        default:
            valid = snapshot.adc_window_sample_count != 0u;
            value = snapshot.adc_clipping_percent;
            break;
        }
        m_rf_alarm_last_value = value;
        m_rf_alarm_last_value_valid = valid;
        uint64_t const timestamp_ns = snapshot.timestamp_ns != 0u
            ? snapshot.timestamp_ns
            : m_tick * 250000000ull;
        bool const was_pending = m_rf_alarm.pending != 0;
        auto const event = dab_measurement_alarm_update(&m_rf_alarm,
                                                         timestamp_ns,
                                                         valid ? 1 : 0,
                                                         value);
        wchar_t value_text[64]{};
        swprintf_s(value_text, L"%.2f %ls", static_cast<double>(value), units[index]);
        auto const event_checked = RfHistoryAlarmAddEventCheckBox().IsChecked();
        bool const add_event = event_checked && event_checked.Value();
        auto const notification_checked = RfHistoryAlarmNotificationCheckBox().IsChecked();
        bool const show_notification = notification_checked && notification_checked.Value();

        if (event == DAB_MEASUREMENT_ALARM_TRIGGERED)
        {
            wchar_t description[180]{};
            swprintf_s(description,
                       L"%ls %.2f %ls for at least %.1f s",
                       m_rf_alarm.config.condition == DAB_MEASUREMENT_ALARM_LESS_THAN
                           ? L"Below" : L"Above",
                       static_cast<double>(m_rf_alarm.config.threshold),
                       units[index],
                       static_cast<double>(m_rf_alarm.config.minimum_duration_ns) / 1000000000.0);
            if (add_event)
            {
                AddRfHistoryEvent(L"Warning", names[index], value_text, description, timestamp_ns);
            }
            AddTimelineEvent(hstring{ L"RF alarm · " } + names[index] + L" " + value_text);
            if (show_notification)
            {
                RfHistoryAlarmBanner().Title(hstring{ names[index] } + L" alarm active");
                RfHistoryAlarmBanner().Message(hstring{ value_text } + L" · " + description);
                RfHistoryAlarmBanner().Severity(InfoBarSeverity::Warning);
                RfHistoryAlarmBanner().IsOpen(true);
            }
            RfHistoryAlarmStateText().Text(hstring{ L"ACTIVE · " } + names[index] + L" " + value_text);
            RfHistoryAlarmStateText().Foreground(Application::Current().Resources().Lookup(
                box_value(hstring{ L"WarningBrush" })).as<Brush>());
        }
        else if (event == DAB_MEASUREMENT_ALARM_RECOVERED)
        {
            wchar_t description[160]{};
            swprintf_s(description,
                       L"Recovered at %ls %.2f %ls",
                       m_rf_alarm.config.condition == DAB_MEASUREMENT_ALARM_LESS_THAN
                           ? L"≥" : L"≤",
                       static_cast<double>(m_rf_alarm.config.recovery_threshold),
                       units[index]);
            if (add_event)
            {
                AddRfHistoryEvent(L"Info", names[index], value_text, description, timestamp_ns);
            }
            RfHistoryAlarmBanner().IsOpen(false);
            RfHistoryAlarmStateText().Text(hstring{ L"Recovered · " } + names[index] + L" " + value_text);
            RfHistoryAlarmStateText().Foreground(Application::Current().Resources().Lookup(
                box_value(hstring{ L"MeasurementAccentBrush" })).as<Brush>());
        }
        else if (m_rf_alarm.pending && valid)
        {
            double const elapsed = static_cast<double>(timestamp_ns - m_rf_alarm.pending_since_ns) /
                                   1000000000.0;
            double const required = static_cast<double>(m_rf_alarm.config.minimum_duration_ns) /
                                    1000000000.0;
            wchar_t pending_text[140]{};
            swprintf_s(pending_text,
                       L"Pending · %ls %ls · %.1f / %.1f s",
                       names[index],
                       value_text,
                       elapsed,
                       required);
            RfHistoryAlarmStateText().Text(pending_text);
            RfHistoryAlarmStateText().Foreground(Application::Current().Resources().Lookup(
                box_value(hstring{ L"WarningBrush" })).as<Brush>());
        }
        else if (was_pending && !m_rf_alarm.active)
        {
            wchar_t armed_text[140]{};
            swprintf_s(armed_text,
                       L"Armed · %ls %ls %.2f %ls for %.1f s",
                       names[index],
                       m_rf_alarm.config.condition == DAB_MEASUREMENT_ALARM_LESS_THAN
                           ? L"<" : L">",
                       static_cast<double>(m_rf_alarm.config.threshold),
                       units[index],
                       static_cast<double>(m_rf_alarm.config.minimum_duration_ns) / 1000000000.0);
            RfHistoryAlarmStateText().Text(armed_text);
            RfHistoryAlarmStateText().Foreground(Application::Current().Resources().Lookup(
                box_value(hstring{ L"MeasurementAccentBrush" })).as<Brush>());
        }
    }

    void MainWindow::RfHistoryAnnotationCommit_Click(IInspectable const&, RoutedEventArgs const&)
    {
        std::wstring note{ RfHistoryAnnotationTextBox().Text().c_str() };
        size_t const first = note.find_first_not_of(L" \t\r\n");
        size_t const last = note.find_last_not_of(L" \t\r\n");
        if (first == std::wstring::npos)
        {
            return;
        }
        note = note.substr(first, last - first + 1u);
        AddRfHistoryEvent(L"Annotation", L"Operator", L"Marker", hstring{ note });
        AddTimelineEvent(hstring{ L"RF annotation · " } + note);
        RfHistoryAnnotationTextBox().Text(L"");
        RfHistoryAnnotationFlyout().Hide();
    }

    void MainWindow::RfHistoryClearEvents_Click(IInspectable const&, RoutedEventArgs const&)
    {
        m_rf_history_events.clear();
        RefreshRfHistoryEventList();
        UpdateRfLongHistoryVisuals();
        AddTimelineEvent(L"RF history events cleared");
    }

    winrt::fire_and_forget MainWindow::RfHistoryExportCsv_Click(
        IInspectable const&,
        RoutedEventArgs const&)
    {
        auto lifetime = get_strong();
        try
        {
            if (m_rf_long_history_count == 0u)
            {
                RfHistoryCaptureStatusText().Text(L"No history samples to export");
                co_return;
            }
            size_t const oldest = (m_rf_long_history_write + rf_long_history_capacity -
                                   m_rf_long_history_count) % rf_long_history_capacity;
            size_t const newest = (m_rf_long_history_write + rf_long_history_capacity - 1u) %
                                  rf_long_history_capacity;
            uint64_t const end_ns = m_rf_history_display_paused &&
                m_rf_history_pause_timestamp_ns != 0u
                ? m_rf_history_pause_timestamp_ns
                : m_rf_long_history[newest].timestamp_ns;
            uint64_t const period_ns = RfHistoryPeriodNs();
            uint64_t const start_ns = period_ns != 0u && end_ns > period_ns
                ? end_ns - period_ns
                : m_rf_long_history[oldest].timestamp_ns;
            std::wstring csv = L"timestamp_utc,elapsed_s,power_dbfs,mer_db,cfo_hz,clipping_percent,sync_locked\r\n";
            uint64_t first_sample_ns = 0u;
            size_t exported = 0u;
            auto value_field = [](bool valid, float value, int precision) -> std::wstring
            {
                if (!valid || !std::isfinite(value))
                {
                    return {};
                }
                wchar_t buffer[48]{};
                swprintf_s(buffer, L"%.*f", precision, static_cast<double>(value));
                return buffer;
            };
            for (size_t offset = 0u; offset < m_rf_long_history_count; ++offset)
            {
                auto const& sample = m_rf_long_history[(oldest + offset) % rf_long_history_capacity];
                if (sample.timestamp_ns < start_ns || sample.timestamp_ns > end_ns)
                {
                    continue;
                }
                if (first_sample_ns == 0u)
                {
                    first_sample_ns = sample.timestamp_ns;
                }
                wchar_t elapsed[48]{};
                swprintf_s(elapsed,
                           L"%.3f",
                           static_cast<double>(sample.timestamp_ns - first_sample_ns) / 1000000000.0);
                csv += history_iso_utc(sample.wall_time_100ns) + L"," + elapsed + L"," +
                    value_field(sample.power_valid, sample.power_dbfs, 3) + L"," +
                    value_field(sample.mer_valid, sample.mer_db, 3) + L"," +
                    value_field(sample.cfo_valid, sample.cfo_hz, 3) + L"," +
                    value_field(sample.clipping_valid, sample.clipping_percent, 6) + L"," +
                    (sample.locked ? L"1" : L"0") + L"\r\n";
                ++exported;
            }

            using namespace Windows::Storage;
            SYSTEMTIME now{};
            GetLocalTime(&now);
            wchar_t filename[80]{};
            swprintf_s(filename,
                       L"DAB_RF_History_%04u%02u%02u_%02u%02u%02u.csv",
                       now.wYear,
                       now.wMonth,
                       now.wDay,
                       now.wHour,
                       now.wMinute,
                       now.wSecond);
            auto const file = co_await DownloadsFolder::CreateFileAsync(
                filename,
                CreationCollisionOption::GenerateUniqueName);
            co_await FileIO::WriteTextAsync(file, hstring{ csv });
            RfHistoryCaptureStatusText().Text(
                L"Exported " + to_hstring(exported) + L" samples · " + file.Path());
            AddRfHistoryEvent(L"Info",
                              L"CSV export",
                              to_hstring(exported) + L" samples",
                              file.Path());
            AddTimelineEvent(L"RF history CSV saved to " + file.Path());
        }
        catch (hresult_error const& error)
        {
            RfHistoryCaptureStatusText().Text(L"CSV export failed · " + error.message());
            AddTimelineEvent(L"RF history CSV export failed: " + error.message());
        }
    }

    void MainWindow::ApplySnapshot(dab_receive_snapshot_t const& snapshot)
    {
        if (m_metrics_stale)
        {
            return;
        }
        DataModeText().Text(L"LIVE METRICS");
        DataSeamText().Text(L"snapshot " + to_hstring(snapshot.sequence));
        StatusSyncText().Text(snapshot.locked != 0 ? L"synchronized" : L"acquiring");
        StatusSyncIndicator().Fill(Application::Current().Resources().Lookup(
            box_value(hstring{ snapshot.locked != 0
                ? L"MeasurementAccentBrush"
                : L"WarningBrush" })).as<Brush>());
        SampleRateText().Text(snapshot.sample_rate_hz != 0u
            ? format_float(static_cast<float>(snapshot.sample_rate_hz) / 1000000.0f, L" MS/s", 3)
            : hstring{ L"— MS/s" });

        SnrValueText().Text(snapshot.snr_valid
            ? format_float(snapshot.snr_db, L" dB")
            : hstring{ L"— dB" });
        CfoValueText().Text(snapshot.cfo_valid
            ? format_float(snapshot.cfo_hz, L" Hz")
            : hstring{ L"— Hz" });
        AdcValueText().Text(snapshot.adc_level_valid
            ? format_float(snapshot.adc_level_dbfs, L" dBFS")
            : hstring{ L"— dBFS" });
        PrsValueText().Text(snapshot.prs_valid
            ? format_float(snapshot.prs_score, L"", 3)
            : hstring{ L"—" });
        CnValueText().Text(snapshot.cn_valid
            ? format_float(snapshot.cn_db, L" dB")
            : hstring{ L"— dB" });
        TimingValueText().Text(snapshot.timing_valid
            ? format_integer(snapshot.timing_error_samples, L" samples")
            : hstring{ L"— samples" });
        DropValueText().Text(to_hstring(snapshot.dropped_blocks));
        StatusOverrunText().Text(snapshot.dropped_blocks == 0u ? L"no overruns" : L"overruns detected");

        constexpr uint32_t fibs_per_frame = 12u;
        bool const have_fic_quality = snapshot.locked != 0 && snapshot.fic_frames_seen != 0u;
        CrcValueText().Text(have_fic_quality
            ? to_hstring(snapshot.fic_valid_fibs_last_frame) + L" / " + to_hstring(fibs_per_frame)
            : hstring{ L"N/A" });
        double const fic_quality = have_fic_quality
            ? std::clamp(static_cast<double>(snapshot.fic_total_valid_fibs) /
                         (static_cast<double>(snapshot.fic_frames_seen) *
                          static_cast<double>(fibs_per_frame)),
                         0.0,
                         1.0)
            : 0.0;
        FicProgress().Value(fic_quality);
        if (have_fic_quality)
        {
            wchar_t quality_text[48]{};
            swprintf_s(quality_text, L"FIC %.1f%%", fic_quality * 100.0);
            BerValueText().Text(quality_text);
        }
        else
        {
            BerValueText().Text(L"N/A");
        }
        SampleCounterText().Text(L"frame " + to_hstring(snapshot.sequence));
        ApplyRfSnapshot(snapshot);
        ApplyDabSnapshot(snapshot);
    }

    void MainWindow::ApplyCatalog(dab_receive_catalog_t const& catalog)
    {
        bool const catalog_changed =
            !m_have_catalog || catalog.sequence != m_latest_catalog.sequence;
        uint32_t appeared = 0u;
        uint32_t disappeared = 0u;
        if (m_have_catalog && catalog_changed)
        {
            auto contains = [](dab_receive_catalog_t const& source,
                               uint32_t sid,
                               uint8_t is_programme)
            {
                for (uint32_t index = 0u;
                     index < source.service_count;
                     ++index)
                {
                    if (source.services[index].service_id == sid &&
                        source.services[index].is_programme == is_programme)
                    {
                        return true;
                    }
                }
                return false;
            };
            for (uint32_t index = 0u; index < catalog.service_count; ++index)
            {
                if (!contains(m_latest_catalog,
                              catalog.services[index].service_id,
                              catalog.services[index].is_programme))
                {
                    ++appeared;
                }
            }
            for (uint32_t index = 0u;
                 index < m_latest_catalog.service_count;
                 ++index)
            {
                if (!contains(catalog,
                              m_latest_catalog.services[index].service_id,
                              m_latest_catalog.services[index].is_programme))
                {
                    ++disappeared;
                }
            }
        }
        uint32_t selected_service_id = 0u;
        bool preserve_selection = false;
        int32_t const old_selected_index = ServicesListView().SelectedIndex();
        if (m_have_catalog && old_selected_index >= 0 &&
            static_cast<uint32_t>(old_selected_index) < m_latest_catalog.service_count)
        {
            selected_service_id =
                m_latest_catalog.services[static_cast<uint32_t>(old_selected_index)].service_id;
            preserve_selection = true;
        }

        if (m_have_catalog && catalog_changed)
        {
            m_previous_catalog = m_latest_catalog;
            m_have_previous_catalog = true;
        }
        m_latest_catalog = catalog;
        m_have_catalog = true;
        if (catalog_changed)
        {
            m_dab_catalog_changed_tick = m_tick;
            SYSTEMTIME now{};
            GetLocalTime(&now);
            wchar_t changed_at[40]{};
            swprintf_s(changed_at,
                       L"%04u-%02u-%02u %02u:%02u:%02u",
                       now.wYear,
                       now.wMonth,
                       now.wDay,
                       now.wHour,
                       now.wMinute,
                       now.wSecond);
            m_fic_database_last_change = changed_at;
            if (catalog.sequence <= 1u)
            {
                m_fic_database_change_summary =
                    L"Initial multiplex database acquired";
            }
            else if (appeared != 0u || disappeared != 0u)
            {
                m_fic_database_change_summary =
                    to_hstring(appeared) + L" service(s) appeared · " +
                    to_hstring(disappeared) + L" disappeared";
            }
            else
            {
                m_fic_database_change_summary =
                    L"Signalling definitions updated without service loss";
            }
        }

        hstring const ensemble_label = fic_label_text(catalog.ensemble_label);
        if (ensemble_label.empty())
        {
            wchar_t fallback[48]{};
            swprintf_s(fallback, L"Ensemble 0x%04X", catalog.ensemble_id);
            EnsembleNameText().Text(fallback);
        }
        else
        {
            EnsembleNameText().Text(ensemble_label);
        }

        wchar_t ensemble_detail[112]{};
        hstring const channel = StatusChannelText().Text();
        swprintf_s(ensemble_detail,
                   L"EId 0x%04X · %s · %u service%s",
                   catalog.ensemble_id,
                   channel.c_str(),
                   catalog.service_count,
                   catalog.service_count == 1u ? L"" : L"s");
        EnsembleDetailText().Text(ensemble_detail);

        auto service_items = ServicesListView().Items();
        service_items.Clear();
        if (catalog.service_count == 0u)
        {
            ListViewItem empty_item;
            empty_item.Content(box_value(L"FIC ensemble decoded; awaiting services..."));
            empty_item.IsEnabled(false);
            service_items.Append(empty_item);
            ServicesListView().SelectedIndex(-1);
            ApplySelectedServiceComponents();
        }
        else
        {
            int32_t selected_index = 0;
            for (uint32_t index = 0u; index < catalog.service_count; ++index)
            {
                dab_receive_service_info_t const& service = catalog.services[index];
                hstring label = fic_label_text(service.label);
                if (label.empty())
                {
                    label = L"Unnamed service";
                }

                StackPanel panel;
                panel.Spacing(1.0);
                panel.Tag(box_value(service.service_id));
                TextBlock title;
                title.Text(label);
                title.TextTrimming(TextTrimming::CharacterEllipsis);
                panel.Children().Append(title);

                wchar_t detail[72]{};
                swprintf_s(detail,
                           L"SID 0x%08X · %u component%s",
                           service.service_id,
                           service.component_count,
                           service.component_count == 1u ? L"" : L"s");
                TextBlock detail_text;
                detail_text.Text(detail);
                detail_text.FontSize(11.0);
                detail_text.Opacity(0.65);
                panel.Children().Append(detail_text);
                service_items.Append(panel);

                if (preserve_selection && service.service_id == selected_service_id)
                {
                    selected_index = static_cast<int32_t>(index);
                }
            }
            ServicesListView().SelectedIndex(selected_index);
            ApplySelectedServiceComponents();
        }

        hstring const timeline_name = EnsembleNameText().Text();
        UpdateDabContext();
        UpdateFicDatabase();
        if (FindMscSubchannel(m_msc_selected_subchannel) == nullptr)
        {
            if (m_msc_have_snapshot &&
                m_msc_latest_snapshot.msc_selected_subchannel_valid != 0 &&
                FindMscSubchannel(
                    m_msc_latest_snapshot.msc_selected_subchannel_id) != nullptr)
            {
                m_msc_selected_subchannel =
                    m_msc_latest_snapshot.msc_selected_subchannel_id;
            }
            else
            {
                for (auto const& subchannel : m_latest_catalog.subchannels)
                {
                    if (subchannel.valid != 0u)
                    {
                        m_msc_selected_subchannel =
                            subchannel.subchannel_id;
                        break;
                    }
                }
            }
        }
        m_msc_live_selection =
            m_msc_have_snapshot &&
            m_msc_latest_snapshot.msc_selected_subchannel_valid != 0 &&
            m_msc_latest_snapshot.msc_selected_subchannel_id ==
                m_msc_selected_subchannel;
        RenderMscCapacityMap();
        RefreshMscSubchannelTable();
        UpdateMscSelectedDetails();
        AddTimelineEvent(L"FIC catalog · " + timeline_name + L" · " +
                         to_hstring(catalog.service_count) + L" services");
    }

    void MainWindow::ApplySelectedServiceComponents()
    {
        auto component_items = ComponentsListView().Items();
        component_items.Clear();
        int32_t const selected_index = ServicesListView().SelectedIndex();
        if (!m_have_catalog || selected_index < 0 ||
            static_cast<uint32_t>(selected_index) >= m_latest_catalog.service_count)
        {
            ListViewItem empty_item;
            empty_item.Content(box_value(L"Select a decoded service"));
            empty_item.IsEnabled(false);
            component_items.Append(empty_item);
            UpdateDabContext();
            return;
        }

        dab_receive_service_info_t const& service =
            m_latest_catalog.services[static_cast<uint32_t>(selected_index)];
        if (service.component_count == 0u)
        {
            ListViewItem empty_item;
            empty_item.Content(box_value(L"No FIC components decoded yet"));
            empty_item.IsEnabled(false);
            component_items.Append(empty_item);
            UpdateDabContext();
            return;
        }

        for (uint32_t index = 0u; index < service.component_count; ++index)
        {
            dab_receive_component_info_t const& component = service.components[index];
            StackPanel panel;
            panel.Spacing(1.0);

            TextBlock title;
            hstring title_text = component_kind_text(component.kind);
            if (component.primary != 0u)
            {
                title_text = title_text + L" · primary";
            }
            title.Text(title_text);
            panel.Children().Append(title);

            wchar_t detail[160]{};
            if (component.has_subchannel != 0u)
            {
                if (component.subchannel_valid != 0u)
                {
                    if (component.protection_kind == 0u)
                    {
                        swprintf_s(detail,
                                   L"SubCh %u · SC type %u · UEP table %u index %u",
                                   component.subchannel_id,
                                   component.service_component_type,
                                   component.protection_table_switch,
                                   component.protection_index);
                    }
                    else
                    {
                        swprintf_s(detail,
                                   L"SubCh %u · SC type %u · %u CU · EEP option %u level %u",
                                   component.subchannel_id,
                                   component.service_component_type,
                                   component.subchannel_size_cu,
                                   component.protection_option,
                                   component.protection_level);
                    }
                }
                else
                {
                    swprintf_s(detail,
                               L"SubCh %u · SC type %u · allocation pending",
                               component.subchannel_id,
                               component.service_component_type);
                }
            }
            else if (component.has_packet_address != 0u)
            {
                swprintf_s(detail,
                           L"SCId 0x%03X · packet 0x%03X · SC type %u",
                           component.service_component_id,
                           component.packet_address,
                           component.service_component_type);
            }
            else
            {
                swprintf_s(detail,
                           L"TMId %u · SC type %u · component %u",
                           component.transport_mode_id,
                           component.service_component_type,
                           component.component_index);
            }
            TextBlock detail_text;
            detail_text.Text(detail);
            detail_text.TextWrapping(TextWrapping::Wrap);
            detail_text.FontSize(11.0);
            detail_text.Opacity(0.65);
            panel.Children().Append(detail_text);
            component_items.Append(panel);
        }
        UpdateDabContext();
    }

    void MainWindow::ClearCatalogView()
    {
        m_have_catalog = false;
        m_latest_catalog = {};
        m_have_previous_catalog = false;
        m_previous_catalog = {};
        m_msc_selected_subchannel = 0xFFu;
        m_msc_live_selection = false;
        m_dab_catalog_changed_tick = m_tick;
        EnsembleNameText().Text(L"Awaiting FIC ensemble");
        EnsembleDetailText().Text(L"Decoding ensemble metadata from the signal");

        auto service_items = ServicesListView().Items();
        service_items.Clear();
        ListViewItem service_placeholder;
        service_placeholder.Content(box_value(L"Awaiting decoded services..."));
        service_placeholder.IsEnabled(false);
        service_items.Append(service_placeholder);
        ServicesListView().SelectedIndex(-1);
        ApplySelectedServiceComponents();
        UpdateDabContext();
        RenderMscCapacityMap();
        RefreshMscSubchannelTable();
        UpdateMscSelectedDetails();
    }

    void MainWindow::UpdateCirPlot(dab::measurement::MeasurementFrame const& frame)
    {
        uint32_t const count = (std::min)(frame.cir_bin_count,
                                          static_cast<uint32_t>(DAB_RECEIVE_CIR_BIN_COUNT));
        if (count == 0u || frame.cir_sample_period_ns <= 0.0f)
        {
            return;
        }

        bool const reset_average = m_cir_frequency_hz != frame.frequency_hz ||
            m_cir_bin_count != count ||
            std::abs(m_cir_sample_period_ns - frame.cir_sample_period_ns) > 0.001f;
        if (reset_average)
        {
            m_cir_power_history_linear = {};
            m_cir_power_sum_linear = {};
            m_cir_power_history_write = 0u;
            m_cir_power_history_count = 0u;
            m_cir_frequency_hz = frame.frequency_hz;
            m_cir_bin_count = count;
            m_cir_sample_period_ns = frame.cir_sample_period_ns;
        }

        size_t const slot = m_cir_power_history_write;
        bool const replacing = m_cir_power_history_count == cir_average_capacity;
        for (uint32_t bin = 0u; bin < count; ++bin)
        {
            float const db = std::clamp(frame.cir_power_db[bin], -120.0f, 0.0f);
            float const linear = std::pow(10.0f, db / 10.0f);
            if (replacing)
            {
                m_cir_power_sum_linear[bin] -= m_cir_power_history_linear[slot][bin];
            }
            m_cir_power_history_linear[slot][bin] = linear;
            m_cir_power_sum_linear[bin] += linear;
        }
        m_cir_power_history_write = (slot + 1u) % cir_average_capacity;
        m_cir_power_history_count = (std::min)(m_cir_power_history_count + 1u,
                                               cir_average_capacity);

        std::array<float, DAB_RECEIVE_CIR_BIN_COUNT> level_db{};
        uint32_t peak_bin = 0u;
        float peak_power = 0.0f;
        float const divisor = static_cast<float>(m_cir_power_history_count);
        for (uint32_t bin = 0u; bin < count; ++bin)
        {
            float const average_power = m_cir_power_sum_linear[bin] / divisor;
            if (average_power > peak_power)
            {
                peak_power = average_power;
                peak_bin = bin;
            }
        }
        peak_power = (std::max)(peak_power, 1.0e-12f);
        for (uint32_t bin = 0u; bin < count; ++bin)
        {
            float const ratio = (m_cir_power_sum_linear[bin] / divisor) / peak_power;
            level_db[bin] = ratio > 1.0e-12f
                ? 10.0f * std::log10(ratio)
                : -120.0f;
        }

        constexpr float plot_left = 58.0f;
        constexpr float plot_top = 34.0f;
        constexpr float plot_width = 820.0f;
        constexpr float plot_height = 166.0f;
        PointCollection cir_points;
        for (uint32_t bin = 0u; bin < count; ++bin)
        {
            float const x = count > 1u
                ? plot_left + plot_width * static_cast<float>(bin) /
                    static_cast<float>(count - 1u)
                : plot_left;
            float const y = plot_top + std::clamp(-level_db[bin] / 60.0f,
                                                  0.0f,
                                                  1.0f) * plot_height;
            cir_points.Append(Point{ x, y });
        }
        CirTrace().Points(cir_points);

        std::vector<float> floor_samples;
        floor_samples.reserve(count);
        for (uint32_t bin = 0u; bin < count; ++bin)
        {
            if (std::abs(static_cast<int32_t>(bin) - static_cast<int32_t>(peak_bin)) > 6)
            {
                floor_samples.push_back(level_db[bin]);
            }
        }
        float floor_db = -60.0f;
        if (!floor_samples.empty())
        {
            size_t const middle = floor_samples.size() / 2u;
            std::nth_element(floor_samples.begin(),
                             floor_samples.begin() + middle,
                             floor_samples.end());
            floor_db = floor_samples[middle];
        }
        float const detection_threshold_db = std::clamp(floor_db + 8.0f, -35.0f, -10.0f);

        struct CirPeak
        {
            uint32_t bin;
            float level_db;
        };
        std::vector<CirPeak> candidates;
        candidates.push_back(CirPeak{ peak_bin, level_db[peak_bin] });
        for (uint32_t bin = 1u; bin + 1u < count; ++bin)
        {
            if (bin != peak_bin && level_db[bin] >= detection_threshold_db &&
                level_db[bin] >= level_db[bin - 1u] &&
                level_db[bin] > level_db[bin + 1u])
            {
                candidates.push_back(CirPeak{ bin, level_db[bin] });
            }
        }
        std::sort(candidates.begin(), candidates.end(), [](CirPeak const& left, CirPeak const& right)
        {
            return left.level_db > right.level_db;
        });

        std::vector<CirPeak> selected;
        for (auto const& candidate : candidates)
        {
            bool separated = true;
            for (auto const& existing : selected)
            {
                if (std::abs(static_cast<int32_t>(candidate.bin) -
                             static_cast<int32_t>(existing.bin)) < 6)
                {
                    separated = false;
                    break;
                }
            }
            if (separated)
            {
                selected.push_back(candidate);
                if (selected.size() == 4u)
                {
                    break;
                }
            }
        }
        std::sort(selected.begin(), selected.end(), [](CirPeak const& left, CirPeak const& right)
        {
            return left.bin < right.bin;
        });

        auto markers = CirMarkerCanvas().Children();
        markers.Clear();
        auto const strongest_brush = SolidColorBrush(ColorHelper::FromArgb(255u, 255u, 200u, 87u));
        auto const secondary_brush = SolidColorBrush(ColorHelper::FromArgb(255u, 84u, 226u, 255u));
        uint32_t secondary_number = 0u;
        std::wstring path_readout{ L"Detected: " };
        for (size_t index = 0u; index < selected.size(); ++index)
        {
            auto const& path = selected[index];
            bool const strongest = path.bin == peak_bin;
            auto const brush = strongest ? strongest_brush : secondary_brush;
            double const x = count > 1u
                ? plot_left + plot_width * static_cast<double>(path.bin) /
                    static_cast<double>(count - 1u)
                : plot_left;
            double const y = plot_top + std::clamp(-static_cast<double>(path.level_db) / 60.0,
                                                   0.0,
                                                   1.0) * plot_height;

            Line marker_line;
            marker_line.X1(x);
            marker_line.X2(x);
            marker_line.Y1(y + 5.0);
            marker_line.Y2(plot_top + plot_height);
            marker_line.Stroke(brush);
            marker_line.StrokeThickness(strongest ? 1.5 : 1.0);
            marker_line.Opacity(strongest ? 0.85 : 0.55);
            markers.Append(marker_line);

            Microsoft::UI::Xaml::Shapes::Ellipse marker_dot;
            marker_dot.Width(strongest ? 7.0 : 6.0);
            marker_dot.Height(strongest ? 7.0 : 6.0);
            marker_dot.Fill(brush);
            Canvas::SetLeft(marker_dot, x - (strongest ? 3.5 : 3.0));
            Canvas::SetTop(marker_dot, y - (strongest ? 3.5 : 3.0));
            markers.Append(marker_dot);

            TextBlock label;
            wchar_t label_text[16]{};
            wchar_t path_text[80]{};
            double const delay_us = static_cast<double>(path.bin) *
                static_cast<double>(frame.cir_sample_period_ns) / 1000.0;
            if (strongest)
            {
                swprintf_s(label_text, L"S");
                swprintf_s(path_text, L"S %.2f µs  0.0 dB", delay_us);
            }
            else
            {
                ++secondary_number;
                swprintf_s(label_text, L"P%u", secondary_number);
                swprintf_s(path_text, L"P%u %.2f µs  %.1f dB",
                           secondary_number,
                           delay_us,
                           static_cast<double>(path.level_db));
            }
            if (index != 0u)
            {
                path_readout += L"   ·   ";
            }
            path_readout += path_text;
            label.Text(label_text);
            label.FontFamily(FontFamily{ L"Consolas" });
            label.FontSize(10.0);
            label.Foreground(brush);
            Canvas::SetLeft(label, std::clamp(x + 4.0, 62.0, 850.0));
            Canvas::SetTop(label, plot_top + 3.0 + 12.0 * static_cast<double>(index % 3u));
            markers.Append(label);
        }
        CirDetectedPathsText().Text(path_readout);

        double const peak_delay_us = static_cast<double>(peak_bin) *
            static_cast<double>(frame.cir_sample_period_ns) / 1000.0;
        wchar_t status[128]{};
        swprintf_s(status,
                   L"Strongest tap %.2f µs (FFT-relative) · %zu/%zu-frame average",
                   peak_delay_us,
                   m_cir_power_history_count,
                   cir_average_capacity);
        CirStatusText().Text(status);

        wchar_t summary[128]{};
        swprintf_s(summary,
                   L"%zu strongest candidate%s · floor %.1f dB",
                   selected.size(),
                   selected.size() == 1u ? L"" : L"s",
                   static_cast<double>(floor_db));
        CirPathSummaryText().Text(summary);
    }

    void MainWindow::UpdateRfChannel(dab::measurement::MeasurementFrame const& frame)
    {
        if (frame.channel_response_count != 0u)
        {
            UpdateRfChannelResponse(frame);
        }
        if (frame.cir_bin_count != 0u && m_cir_power_history_count != 0u)
        {
            UpdateRfChannelCir(frame);
        }
    }

    void MainWindow::UpdateRfChannelResponse(dab::measurement::MeasurementFrame const& frame)
    {
        uint32_t const response_count = (std::min)(
            frame.channel_response_count,
            static_cast<uint32_t>(DAB_RECEIVE_CHANNEL_RESPONSE_COUNT));
        if (response_count == 0u)
        {
            return;
        }

        constexpr uint32_t display_points = 193u;
        constexpr float plot_left = 48.0f;
        constexpr float plot_width = 584.0f;
        PointCollection magnitude_points;
        PointCollection phase_points;
        PointCollection mer_points;
        for (uint32_t point = 0u; point < display_points; ++point)
        {
            uint32_t const begin = (point * response_count) / display_points;
            uint32_t const end = (std::max)(begin + 1u,
                ((point + 1u) * response_count) / display_points);
            double magnitude_sum = 0.0;
            double mer_sum = 0.0;
            uint32_t magnitude_used = 0u;
            uint32_t mer_used = 0u;
            for (uint32_t carrier = begin; carrier < (std::min)(end, response_count); ++carrier)
            {
                if (std::isfinite(frame.channel_magnitude_db[carrier]))
                {
                    magnitude_sum += frame.channel_magnitude_db[carrier];
                    ++magnitude_used;
                }
                if (carrier < frame.carrier_mer_count &&
                    std::isfinite(frame.carrier_mer_db[carrier]))
                {
                    mer_sum += frame.carrier_mer_db[carrier];
                    ++mer_used;
                }
            }
            uint32_t const phase_index = (std::min)(
                begin + ((std::min)(end, response_count) - begin) / 2u,
                response_count - 1u);
            float const magnitude = magnitude_used != 0u
                ? static_cast<float>(magnitude_sum / static_cast<double>(magnitude_used))
                : -20.0f;
            float const phase = std::clamp(frame.channel_phase_deg[phase_index], -180.0f, 180.0f);
            float const mer = mer_used != 0u
                ? static_cast<float>(mer_sum / static_cast<double>(mer_used))
                : 0.0f;
            float const x = plot_left + plot_width * static_cast<float>(point) /
                static_cast<float>(display_points - 1u);
            float const magnitude_y = 20.0f +
                (10.0f - std::clamp(magnitude, -20.0f, 10.0f)) / 30.0f * 99.0f;
            float const phase_y = 148.0f +
                (180.0f - phase) / 360.0f * 66.0f;
            float const mer_y = 243.0f +
                (40.0f - std::clamp(mer, 0.0f, 40.0f)) / 40.0f * 66.0f;
            magnitude_points.Append(Point{ x, magnitude_y });
            phase_points.Append(Point{ x, phase_y });
            mer_points.Append(Point{ x, mer_y });
        }
        RfChannelMagnitudeTrace().Points(magnitude_points);
        RfChannelPhaseTrace().Points(phase_points);
        RfChannelMerTrace().Points(mer_points);

        std::vector<float> magnitudes;
        magnitudes.reserve(response_count);
        uint32_t notched_carriers = 0u;
        for (uint32_t carrier = 0u; carrier < response_count; ++carrier)
        {
            float const magnitude = frame.channel_magnitude_db[carrier];
            if (!std::isfinite(magnitude))
            {
                continue;
            }
            magnitudes.push_back(std::clamp(magnitude, -60.0f, 20.0f));
            if (magnitude < -12.0f)
            {
                ++notched_carriers;
            }
        }
        std::sort(magnitudes.begin(), magnitudes.end());
        float spread_db = 0.0f;
        if (!magnitudes.empty())
        {
            size_t const p10_index = static_cast<size_t>(
                0.10 * static_cast<double>(magnitudes.size() - 1u));
            size_t const p90_index = static_cast<size_t>(
                0.90 * static_cast<double>(magnitudes.size() - 1u));
            spread_db = magnitudes[p90_index] - magnitudes[p10_index];
        }
        wchar_t const* fading = spread_db < 3.0f ? L"Flat" :
            spread_db < 8.0f ? L"Mild" :
            spread_db < 15.0f ? L"Moderate" : L"Severe";
        RfChannelNotchedCarrierText().Text(to_hstring(notched_carriers));
        RfChannelFadingText().Text(fading);
        RfChannelFadingSpreadText().Text(
            hstring{ L"p90\x2212p10 " } + format_float(spread_db, L" dB"));

        uint32_t const mer_count = (std::min)(frame.carrier_mer_count, response_count);
        double mer_sum = 0.0;
        uint32_t mer_used = 0u;
        float minimum_mer = std::numeric_limits<float>::infinity();
        for (uint32_t carrier = 0u; carrier < mer_count; ++carrier)
        {
            float const mer = frame.carrier_mer_db[carrier];
            if (!std::isfinite(mer))
            {
                continue;
            }
            mer_sum += mer;
            minimum_mer = (std::min)(minimum_mer, mer);
            ++mer_used;
        }
        RfChannelOverallMerText().Text(mer_used != 0u
            ? format_float(static_cast<float>(mer_sum / static_cast<double>(mer_used)), L" dB")
            : hstring{ L"\x2014 dB" });
        RfChannelMinimumMerText().Text(mer_used != 0u
            ? format_float(minimum_mer, L" dB")
            : hstring{ L"\x2014 dB" });

        wchar_t response_status[160]{};
        swprintf_s(response_status,
                   L"%u carriers \u00B7 %.0f Hz spacing \u00B7 magnitude normalized to mean carrier power \u00B7 phase wrapped \u00B1180\u00B0",
                   response_count,
                   static_cast<double>(frame.carrier_spacing_hz));
        RfChannelResponseStatusText().Text(response_status);
    }

    void MainWindow::UpdateRfChannelCir(dab::measurement::MeasurementFrame const& frame)
    {
        uint32_t const count = (std::min)(m_cir_bin_count,
                                          static_cast<uint32_t>(DAB_RECEIVE_CIR_BIN_COUNT));
        if (count == 0u || m_cir_power_history_count == 0u ||
            frame.cir_sample_period_ns <= 0.0f)
        {
            return;
        }

        float const divisor = static_cast<float>(m_cir_power_history_count);
        uint32_t main_bin = 0u;
        float main_power = 0.0f;
        for (uint32_t bin = 0u; bin < count; ++bin)
        {
            float const power = m_cir_power_sum_linear[bin] / divisor;
            if (power > main_power)
            {
                main_power = power;
                main_bin = bin;
            }
        }
        main_power = (std::max)(main_power, 1.0e-12f);
        m_rf_channel_cir_count = count;
        m_rf_channel_cir_main_bin = main_bin;
        m_rf_channel_cir_sample_period_ns = frame.cir_sample_period_ns;

        constexpr float plot_left = 52.0f;
        constexpr float plot_top = 30.0f;
        constexpr float plot_width = 580.0f;
        constexpr float plot_height = 160.0f;
        PointCollection cir_points;
        for (uint32_t delay = 0u; delay < count; ++delay)
        {
            uint32_t const bin = (main_bin + delay) % count;
            float const ratio = (m_cir_power_sum_linear[bin] / divisor) / main_power;
            float const level = ratio > 1.0e-12f ? 10.0f * std::log10(ratio) : -120.0f;
            m_rf_channel_cir_level_db[delay] = level;
            float const x = count > 1u
                ? plot_left + plot_width * static_cast<float>(delay) /
                    static_cast<float>(count - 1u)
                : plot_left;
            float const y = plot_top + std::clamp(-level / 60.0f, 0.0f, 1.0f) * plot_height;
            cir_points.Append(Point{ x, y });
        }
        RfChannelCirTrace().Points(cir_points);

        std::vector<float> floor_samples;
        floor_samples.reserve(count);
        for (uint32_t delay = 7u; delay < count; ++delay)
        {
            floor_samples.push_back(m_rf_channel_cir_level_db[delay]);
        }
        float floor_db = -60.0f;
        if (!floor_samples.empty())
        {
            size_t const middle = floor_samples.size() / 2u;
            std::nth_element(floor_samples.begin(),
                             floor_samples.begin() + middle,
                             floor_samples.end());
            floor_db = floor_samples[middle];
        }
        float const detection_threshold_db = std::clamp(floor_db + 8.0f, -35.0f, -10.0f);

        struct EchoPeak
        {
            uint32_t delay;
            float level_db;
        };
        std::vector<EchoPeak> candidates;
        for (uint32_t delay = 6u; delay + 1u < count; ++delay)
        {
            float const level = m_rf_channel_cir_level_db[delay];
            if (level >= detection_threshold_db &&
                level >= m_rf_channel_cir_level_db[delay - 1u] &&
                level > m_rf_channel_cir_level_db[delay + 1u])
            {
                candidates.push_back(EchoPeak{ delay, level });
            }
        }
        std::sort(candidates.begin(), candidates.end(), [](EchoPeak const& left, EchoPeak const& right)
        {
            return left.level_db > right.level_db;
        });
        std::vector<EchoPeak> echoes;
        for (auto const& candidate : candidates)
        {
            bool separated = true;
            for (auto const& existing : echoes)
            {
                if (std::abs(static_cast<int32_t>(candidate.delay) -
                             static_cast<int32_t>(existing.delay)) < 6)
                {
                    separated = false;
                    break;
                }
            }
            if (separated)
            {
                echoes.push_back(candidate);
                if (echoes.size() == 3u)
                {
                    break;
                }
            }
        }
        std::sort(echoes.begin(), echoes.end(), [](EchoPeak const& left, EchoPeak const& right)
        {
            return left.delay < right.delay;
        });

        constexpr uint32_t mode_i_guard_samples = 504u;
        double const sample_period_us = static_cast<double>(frame.cir_sample_period_ns) / 1000.0;
        double const guard_us = sample_period_us * static_cast<double>(mode_i_guard_samples);
        auto cir_markers = RfChannelCirAutoMarkerCanvas().Children();
        cir_markers.Clear();
        auto guard_markers = RfChannelGuardMarkerCanvas().Children();
        guard_markers.Clear();
        auto const echo_brush = SolidColorBrush(ColorHelper::FromArgb(255u, 84u, 226u, 255u));
        auto const outside_brush = SolidColorBrush(ColorHelper::FromArgb(255u, 255u, 200u, 87u));
        std::array<hstring, 3> echo_text{};
        for (size_t index = 0u; index < echoes.size(); ++index)
        {
            auto const& echo = echoes[index];
            bool const outside_guard = echo.delay >= mode_i_guard_samples;
            auto const brush = outside_guard ? outside_brush : echo_brush;
            double const delay_us = static_cast<double>(echo.delay) * sample_period_us;
            double const x = plot_left + plot_width * static_cast<double>(echo.delay) /
                static_cast<double>(count - 1u);
            double const y = plot_top +
                std::clamp(-static_cast<double>(echo.level_db) / 60.0, 0.0, 1.0) * plot_height;

            Line marker_line;
            marker_line.X1(x);
            marker_line.X2(x);
            marker_line.Y1(y + 5.0);
            marker_line.Y2(plot_top + plot_height);
            marker_line.Stroke(brush);
            marker_line.StrokeThickness(1.0);
            marker_line.Opacity(0.7);
            cir_markers.Append(marker_line);
            Microsoft::UI::Xaml::Shapes::Ellipse marker_dot;
            marker_dot.Width(7.0);
            marker_dot.Height(7.0);
            marker_dot.Fill(brush);
            Canvas::SetLeft(marker_dot, x - 3.5);
            Canvas::SetTop(marker_dot, y - 3.5);
            cir_markers.Append(marker_dot);
            TextBlock label;
            label.Text(hstring{ L"E" } + to_hstring(index + 1u));
            label.FontFamily(FontFamily{ L"Consolas" });
            label.FontSize(10.0);
            label.Foreground(brush);
            Canvas::SetLeft(label, std::clamp(x + 4.0, 56.0, 612.0));
            Canvas::SetTop(label, plot_top + 3.0 + 12.0 * static_cast<double>(index));
            cir_markers.Append(label);

            double const guard_x = 50.0 + 900.0 * delay_us / (std::max)(guard_us, 1.0);
            Line guard_line;
            guard_line.X1(std::clamp(guard_x, 50.0, 972.0));
            guard_line.X2(std::clamp(guard_x, 50.0, 972.0));
            guard_line.Y1(22.0);
            guard_line.Y2(55.0);
            guard_line.Stroke(brush);
            guard_line.StrokeThickness(2.0);
            guard_markers.Append(guard_line);
            TextBlock guard_label;
            guard_label.Text(hstring{ L"E" } + to_hstring(index + 1u) + L" " +
                format_float(static_cast<float>(delay_us), L" \x00B5s", 1));
            guard_label.FontFamily(FontFamily{ L"Consolas" });
            guard_label.FontSize(10.0);
            guard_label.Foreground(brush);
            Canvas::SetLeft(guard_label, std::clamp(guard_x - 22.0, 55.0, 930.0));
            Canvas::SetTop(guard_label, 55.0 + 12.0 * static_cast<double>(index % 2u));
            guard_markers.Append(guard_label);

            wchar_t buffer[104]{};
            swprintf_s(buffer,
                       L"E%zu  %u samp \x00B7 %.2f \x00B5s \x00B7 %.1f dB",
                       index + 1u,
                       echo.delay,
                       delay_us,
                       static_cast<double>(echo.level_db));
            echo_text[index] = buffer;
        }
        RfChannelEcho1Text().Text(echoes.empty() ? hstring{ L"None detected" } : echo_text[0u]);
        RfChannelEcho2Text().Text(echoes.size() > 1u ? echo_text[1u] : hstring{});
        RfChannelEcho3Text().Text(echoes.size() > 2u ? echo_text[2u] : hstring{});
        RfChannelMainPathText().Text(L"0 samp \x00B7 0.00 \x00B5s \x00B7 0.0 dB");
        RfChannelCirAverageText().Text(
            to_hstring(m_cir_power_history_count) + L"/" +
            to_hstring(cir_average_capacity) + L"-frame linear-power average");

        double total_energy = 0.0;
        double outside_energy = 0.0;
        for (uint32_t delay = 0u; delay < count; ++delay)
        {
            uint32_t const bin = (main_bin + delay) % count;
            double const power = static_cast<double>(m_cir_power_sum_linear[bin]) /
                                 static_cast<double>(divisor);
            total_energy += power;
            if (delay >= mode_i_guard_samples)
            {
                outside_energy += power;
            }
        }
        double const outside_ratio = total_energy > 0.0 ? outside_energy / total_energy : 0.0;
        double const outside_percent = outside_ratio * 100.0;
        double const outside_db = outside_ratio > 1.0e-12
            ? 10.0 * std::log10(outside_ratio)
            : -120.0;
        RfChannelOutsideGuardText().Text(
            format_float(static_cast<float>(outside_percent), L" %", 3));
        RfChannelOutsideGuardDbText().Text(
            format_float(static_cast<float>(outside_db), L" dB energy ratio", 1));
        RfChannelGuardStatusText().Text(
            L"Mode I guard interval: " + to_hstring(mode_i_guard_samples) + L" samples / " +
            format_float(static_cast<float>(guard_us), L" \x00B5s", 2));

        if (m_rf_channel_cir_marker_delay_samples >= static_cast<int32_t>(count))
        {
            m_rf_channel_cir_marker_delay_samples = -1;
        }
        RenderRfChannelCirMarker();
    }

    void MainWindow::RfChannelCirCanvas_PointerPressed(IInspectable const&,
                                                        PointerRoutedEventArgs const& args)
    {
        if (m_rf_channel_cir_count < 2u)
        {
            return;
        }
        Point const position = args.GetCurrentPoint(RfChannelCirCanvas()).Position();
        constexpr double plot_left = 52.0;
        constexpr double plot_width = 580.0;
        if (position.X < plot_left || position.X > plot_left + plot_width)
        {
            return;
        }
        uint32_t const approximate = static_cast<uint32_t>(std::llround(
            (position.X - plot_left) / plot_width *
            static_cast<double>(m_rf_channel_cir_count - 1u)));
        uint32_t const begin = approximate > 10u ? approximate - 10u : 0u;
        uint32_t const end = (std::min)(m_rf_channel_cir_count - 1u, approximate + 10u);
        uint32_t best = approximate;
        float best_level = -121.0f;
        for (uint32_t delay = begin; delay <= end; ++delay)
        {
            bool const local_peak = delay == 0u || delay + 1u == m_rf_channel_cir_count ||
                (m_rf_channel_cir_level_db[delay] >= m_rf_channel_cir_level_db[delay - 1u] &&
                 m_rf_channel_cir_level_db[delay] > m_rf_channel_cir_level_db[delay + 1u]);
            if (local_peak && m_rf_channel_cir_level_db[delay] > best_level)
            {
                best = delay;
                best_level = m_rf_channel_cir_level_db[delay];
            }
        }
        m_rf_channel_cir_marker_delay_samples = static_cast<int32_t>(best);
        RenderRfChannelCirMarker();
        args.Handled(true);
    }

    void MainWindow::RenderRfChannelCirMarker()
    {
        auto markers = RfChannelCirUserMarkerCanvas().Children();
        markers.Clear();
        if (m_rf_channel_cir_marker_delay_samples < 0 || m_rf_channel_cir_count < 2u ||
            m_rf_channel_cir_sample_period_ns <= 0.0f)
        {
            RfChannelCirMarkerPanel().Visibility(Visibility::Collapsed);
            return;
        }

        uint32_t const delay = static_cast<uint32_t>(m_rf_channel_cir_marker_delay_samples);
        float const level_db = m_rf_channel_cir_level_db[delay];
        double const delay_us = static_cast<double>(delay) *
            static_cast<double>(m_rf_channel_cir_sample_period_ns) / 1000.0;
        double const path_difference_km = delay_us * 0.299792458;
        double const x = 52.0 + 580.0 * static_cast<double>(delay) /
            static_cast<double>(m_rf_channel_cir_count - 1u);
        double const y = 30.0 +
            std::clamp(-static_cast<double>(level_db) / 60.0, 0.0, 1.0) * 160.0;
        auto const marker_brush = SolidColorBrush(ColorHelper::FromArgb(255u, 53u, 208u, 186u));
        Line marker_line;
        marker_line.X1(x);
        marker_line.X2(x);
        marker_line.Y1(25.0);
        marker_line.Y2(194.0);
        marker_line.Stroke(marker_brush);
        marker_line.StrokeThickness(1.8);
        markers.Append(marker_line);
        Microsoft::UI::Xaml::Shapes::Ellipse dot;
        dot.Width(9.0);
        dot.Height(9.0);
        dot.Fill(marker_brush);
        Canvas::SetLeft(dot, x - 4.5);
        Canvas::SetTop(dot, y - 4.5);
        markers.Append(dot);

        RfChannelMarkerSamplesText().Text(to_hstring(delay) + L" samples");
        RfChannelMarkerDelayText().Text(
            format_float(static_cast<float>(delay_us), L" \x00B5s", 2));
        RfChannelMarkerPowerText().Text(format_float(level_db, L" dB", 1));
        RfChannelMarkerDistanceText().Text(
            format_float(static_cast<float>(path_difference_km), L" km path difference", 2));
        RfChannelCirMarkerPanel().Visibility(Visibility::Visible);
    }

    void MainWindow::ResetRfChannelDiagnostics()
    {
        m_cir_power_history_linear = {};
        m_cir_power_sum_linear = {};
        m_cir_power_history_write = 0u;
        m_cir_power_history_count = 0u;
        m_cir_frequency_hz = 0u;
        m_cir_bin_count = 0u;
        m_cir_sample_period_ns = 0.0f;
        m_rf_channel_cir_level_db = {};
        m_rf_channel_cir_count = 0u;
        m_rf_channel_cir_main_bin = 0u;
        m_rf_channel_cir_sample_period_ns = 0.0f;
        m_rf_channel_cir_marker_delay_samples = -1;
        PointCollection magnitude_points;
        magnitude_points.Append(Point{ 48.0f, 53.0f });
        magnitude_points.Append(Point{ 632.0f, 53.0f });
        RfChannelMagnitudeTrace().Points(magnitude_points);
        PointCollection phase_points;
        phase_points.Append(Point{ 48.0f, 181.0f });
        phase_points.Append(Point{ 632.0f, 181.0f });
        RfChannelPhaseTrace().Points(phase_points);
        PointCollection mer_points;
        mer_points.Append(Point{ 48.0f, 309.0f });
        mer_points.Append(Point{ 632.0f, 309.0f });
        RfChannelMerTrace().Points(mer_points);
        PointCollection cir_points;
        cir_points.Append(Point{ 52.0f, 190.0f });
        cir_points.Append(Point{ 632.0f, 190.0f });
        RfChannelCirTrace().Points(cir_points);
        RfChannelCirAutoMarkerCanvas().Children().Clear();
        RfChannelCirUserMarkerCanvas().Children().Clear();
        RfChannelGuardMarkerCanvas().Children().Clear();
        RfChannelResponseStatusText().Text(L"Awaiting synchronized PRS channel estimate");
        RfChannelMainPathText().Text(L"Awaiting CIR");
        RfChannelEcho1Text().Text(L"None detected");
        RfChannelEcho2Text().Text(L"");
        RfChannelEcho3Text().Text(L"");
        RfChannelOverallMerText().Text(L"\x2014 dB");
        RfChannelMinimumMerText().Text(L"\x2014 dB");
        RfChannelNotchedCarrierText().Text(L"0");
        RfChannelFadingText().Text(L"Awaiting response");
        RfChannelFadingSpreadText().Text(L"p90\x2212p10 \x2014 dB");
        RfChannelOutsideGuardText().Text(L"\x2014 %");
        RfChannelOutsideGuardDbText().Text(L"\x2014 dB energy ratio");
        RfChannelCirMarkerPanel().Visibility(Visibility::Collapsed);
    }

    void MainWindow::UpdateRfSpectrum(dab::measurement::MeasurementFrame const& frame)
    {
        if (m_rf_spectrum_frozen || frame.spectrum_bin_count == 0u)
        {
            return;
        }

        uint32_t const count = (std::min)(frame.spectrum_bin_count,
                                          static_cast<uint32_t>(DAB_RECEIVE_SPECTRUM_BIN_COUNT));
        double const bin_hz = frame.spectrum_bin_hz > 0.0f
            ? static_cast<double>(frame.spectrum_bin_hz)
            : static_cast<double>(frame.sample_rate_hz) / static_cast<double>(count);
        double const center_hz = static_cast<double>(frame.frequency_hz);
        double const source_start_hz = center_hz + static_cast<double>(frame.spectrum_start_hz);
        bool const source_changed = m_rf_spectrum_count != count ||
            m_rf_spectrum_sample_rate_hz != frame.sample_rate_hz ||
            std::abs(m_rf_spectrum_source_center_hz - center_hz) > (std::max)(bin_hz, 1.0);

        if (source_changed)
        {
            m_rf_spectrum_average_initialized = false;
        }
        m_rf_spectrum_count = count;
        m_rf_spectrum_sample_rate_hz = frame.sample_rate_hz;
        m_rf_spectrum_source_center_hz = center_hz;
        m_rf_spectrum_source_start_hz = source_start_hz;
        m_rf_spectrum_bin_hz = bin_hz;

        float const alpha = 1.0f / static_cast<float>((std::max)(m_rf_spectrum_average_factor, 1u));
        for (uint32_t bin = 0u; bin < count; ++bin)
        {
            float const sample = std::isfinite(frame.spectrum_dbfs[bin])
                ? frame.spectrum_dbfs[bin]
                : -120.0f;
            if (!m_rf_spectrum_average_initialized)
            {
                m_rf_spectrum_average[bin] = sample;
                m_rf_spectrum_peak[bin] = sample;
            }
            else
            {
                m_rf_spectrum_average[bin] += alpha * (sample - m_rf_spectrum_average[bin]);
                m_rf_spectrum_peak[bin] = m_rf_spectrum_peak_hold
                    ? (std::max)(m_rf_spectrum_peak[bin], m_rf_spectrum_average[bin])
                    : m_rf_spectrum_average[bin];
            }
        }
        m_rf_spectrum_average_initialized = true;

        if (source_changed)
        {
            ResetRfSpectrumView();
        }
        else
        {
            RenderRfSpectrum();
            AppendRfSpectrumWaterfall();
        }
    }

    void MainWindow::ResetRfSpectrumView()
    {
        m_rf_spectrum_view_center_hz = m_rf_spectrum_source_center_hz;
        m_rf_spectrum_view_span_hz = m_rf_spectrum_sample_rate_hz != 0u
            ? static_cast<double>(m_rf_spectrum_sample_rate_hz)
            : 2048000.0;
        ClampRfSpectrumView();
        if (m_rf_spectrum_controls_ready)
        {
            RfSpectrumWaterfallCanvas().Children().Clear();
            UpdateRfSpectrumReadouts();
            RenderRfSpectrum();
            AppendRfSpectrumWaterfall();
        }
    }

    void MainWindow::ClampRfSpectrumView()
    {
        double const source_span = m_rf_spectrum_sample_rate_hz != 0u
            ? static_cast<double>(m_rf_spectrum_sample_rate_hz)
            : 2048000.0;
        m_rf_spectrum_view_span_hz = std::clamp(m_rf_spectrum_view_span_hz,
                                                20000.0,
                                                source_span);
        double const source_left = m_rf_spectrum_source_center_hz - source_span * 0.5;
        double const source_right = m_rf_spectrum_source_center_hz + source_span * 0.5;
        double const half_span = m_rf_spectrum_view_span_hz * 0.5;
        m_rf_spectrum_view_center_hz = std::clamp(m_rf_spectrum_view_center_hz,
                                                  source_left + half_span,
                                                  source_right - half_span);
    }

    double MainWindow::RfSpectrumFrequencyFromX(double x) const noexcept
    {
        double const ratio = std::clamp(x / 1000.0, 0.0, 1.0);
        return m_rf_spectrum_view_center_hz + (ratio - 0.5) * m_rf_spectrum_view_span_hz;
    }

    double MainWindow::RfSpectrumXFromFrequency(double frequency_hz) const noexcept
    {
        if (m_rf_spectrum_view_span_hz <= 0.0)
        {
            return 0.0;
        }
        return 1000.0 * (frequency_hz -
            (m_rf_spectrum_view_center_hz - m_rf_spectrum_view_span_hz * 0.5)) /
            m_rf_spectrum_view_span_hz;
    }

    float MainWindow::RfSpectrumPowerAtFrequency(double frequency_hz) const noexcept
    {
        if (m_rf_spectrum_count == 0u || m_rf_spectrum_bin_hz <= 0.0)
        {
            return -120.0f;
        }
        double const fractional = std::clamp(
            (frequency_hz - m_rf_spectrum_source_start_hz) / m_rf_spectrum_bin_hz,
            0.0,
            static_cast<double>(m_rf_spectrum_count - 1u));
        uint32_t const lower = static_cast<uint32_t>(std::floor(fractional));
        uint32_t const upper = (std::min)(lower + 1u, m_rf_spectrum_count - 1u);
        float const blend = static_cast<float>(fractional - static_cast<double>(lower));
        return m_rf_spectrum_average[lower] +
            blend * (m_rf_spectrum_average[upper] - m_rf_spectrum_average[lower]);
    }

    void MainWindow::SetRfSpectrumMarker(uint32_t marker, double frequency_hz)
    {
        if (marker >= 2u)
        {
            return;
        }
        double const source_span = m_rf_spectrum_sample_rate_hz != 0u
            ? static_cast<double>(m_rf_spectrum_sample_rate_hz)
            : 2048000.0;
        m_rf_spectrum_marker_frequency_hz[marker] = std::clamp(
            frequency_hz,
            m_rf_spectrum_source_center_hz - source_span * 0.5,
            m_rf_spectrum_source_center_hz + source_span * 0.5);
        m_rf_spectrum_marker_power_dbfs[marker] = RfSpectrumPowerAtFrequency(
            m_rf_spectrum_marker_frequency_hz[marker]);
        m_rf_spectrum_marker_valid[marker] = true;
        m_rf_spectrum_active_marker = marker;
        RenderRfSpectrum();
        UpdateRfSpectrumReadouts();
    }

    void MainWindow::UpdateRfSpectrumHover(Point const& position)
    {
        if (!m_rf_spectrum_controls_ready)
        {
            return;
        }
        m_rf_spectrum_cursor_frequency_hz = RfSpectrumFrequencyFromX(position.X);
        float const power = RfSpectrumPowerAtFrequency(m_rf_spectrum_cursor_frequency_hz);
        wchar_t text[80]{};
        swprintf_s(text,
                   L"%.6f MHz · %.1f dBFS",
                   m_rf_spectrum_cursor_frequency_hz / 1000000.0,
                   static_cast<double>(power));
        RfSpectrumTooltipText().Text(text);
        RfSpectrumTooltipBorder().Visibility(Visibility::Visible);
        Canvas::SetLeft(RfSpectrumTooltipBorder(),
                        std::clamp(static_cast<double>(position.X) + 12.0, 0.0, 830.0));
        Canvas::SetTop(RfSpectrumTooltipBorder(),
                       std::clamp(static_cast<double>(position.Y) - 34.0, 0.0, 270.0));
    }

    void MainWindow::UpdateRfSpectrumReadouts()
    {
        if (!m_rf_spectrum_controls_ready)
        {
            return;
        }

        m_rf_spectrum_updating_controls = true;
        RfSpectrumCenterNumberBox().Value(m_rf_spectrum_view_center_hz / 1000000.0);
        RfSpectrumReferenceNumberBox().Value(m_rf_spectrum_reference_dbfs);

        std::array<double, 5> const spans{ 2048000.0, 1000000.0, 500000.0, 200000.0, 100000.0 };
        int32_t span_index = -1;
        for (uint32_t index = 0u; index < spans.size(); ++index)
        {
            if (std::abs(m_rf_spectrum_view_span_hz - spans[index]) < 1.0)
            {
                span_index = static_cast<int32_t>(index);
                break;
            }
        }
        if (span_index >= 0)
        {
            RfSpectrumSpanComboBox().SelectedIndex(span_index);
        }
        else
        {
            wchar_t custom_span[32]{};
            if (m_rf_spectrum_view_span_hz >= 1000000.0)
            {
                swprintf_s(custom_span, L"%.3f MHz", m_rf_spectrum_view_span_hz / 1000000.0);
            }
            else
            {
                swprintf_s(custom_span, L"%.0f kHz", m_rf_spectrum_view_span_hz / 1000.0);
            }
            RfSpectrumCustomSpanItem().Content(box_value(hstring{ custom_span }));
            RfSpectrumSpanComboBox().SelectedIndex(5);
        }
        m_rf_spectrum_updating_controls = false;

        wchar_t rbw[40]{};
        swprintf_s(rbw, L"%.3f kHz", m_rf_spectrum_bin_hz / 1000.0);
        RfSpectrumRbwText().Text(rbw);

        wchar_t view[80]{};
        if (m_rf_spectrum_view_span_hz >= 1000000.0)
        {
            swprintf_s(view,
                       L"%.6f MHz · %.3f MHz span",
                       m_rf_spectrum_view_center_hz / 1000000.0,
                       m_rf_spectrum_view_span_hz / 1000000.0);
        }
        else
        {
            swprintf_s(view,
                       L"%.6f MHz · %.0f kHz span",
                       m_rf_spectrum_view_center_hz / 1000000.0,
                       m_rf_spectrum_view_span_hz / 1000.0);
        }
        RfSpectrumViewText().Text(view);

        auto set_frequency_text = [](TextBlock const& target, double frequency_hz)
        {
            wchar_t value[32]{};
            swprintf_s(value, L"%.6f MHz", frequency_hz / 1000000.0);
            target.Text(value);
        };
        set_frequency_text(RfSpectrumAxisLeftText(),
                           m_rf_spectrum_view_center_hz - m_rf_spectrum_view_span_hz * 0.5);
        set_frequency_text(RfSpectrumAxisCenterText(), m_rf_spectrum_view_center_hz);
        set_frequency_text(RfSpectrumAxisRightText(),
                           m_rf_spectrum_view_center_hz + m_rf_spectrum_view_span_hz * 0.5);

        auto set_db_text = [](TextBlock const& target, double value, bool suffix)
        {
            wchar_t text[32]{};
            if (suffix)
            {
                swprintf_s(text, L"%.0f dBFS", value);
            }
            else
            {
                swprintf_s(text, L"%.0f", value);
            }
            target.Text(text);
        };
        set_db_text(RfSpectrumYAxisTopText(), m_rf_spectrum_reference_dbfs, false);
        set_db_text(RfSpectrumYAxis1Text(), m_rf_spectrum_reference_dbfs - 2.0 * m_rf_spectrum_scale_db, false);
        set_db_text(RfSpectrumYAxis2Text(), m_rf_spectrum_reference_dbfs - 4.0 * m_rf_spectrum_scale_db, false);
        set_db_text(RfSpectrumYAxisBottomText(), m_rf_spectrum_reference_dbfs - 6.0 * m_rf_spectrum_scale_db, true);

        for (uint32_t marker = 0u; marker < 2u; ++marker)
        {
            if (m_rf_spectrum_marker_valid[marker])
            {
                m_rf_spectrum_marker_power_dbfs[marker] = RfSpectrumPowerAtFrequency(
                    m_rf_spectrum_marker_frequency_hz[marker]);
            }
        }
        wchar_t marker_text[220]{};
        if (m_rf_spectrum_marker_valid[0] && m_rf_spectrum_marker_valid[1])
        {
            swprintf_s(marker_text,
                       L"M1 %.6f MHz  %.1f dBFS  ·  M2 %.6f MHz  %.1f dBFS  ·  Δ %.3f kHz  %+.1f dB",
                       m_rf_spectrum_marker_frequency_hz[0] / 1000000.0,
                       static_cast<double>(m_rf_spectrum_marker_power_dbfs[0]),
                       m_rf_spectrum_marker_frequency_hz[1] / 1000000.0,
                       static_cast<double>(m_rf_spectrum_marker_power_dbfs[1]),
                       (m_rf_spectrum_marker_frequency_hz[1] - m_rf_spectrum_marker_frequency_hz[0]) / 1000.0,
                       static_cast<double>(m_rf_spectrum_marker_power_dbfs[1] -
                                           m_rf_spectrum_marker_power_dbfs[0]));
        }
        else if (m_rf_spectrum_marker_valid[m_rf_spectrum_active_marker])
        {
            uint32_t const marker = m_rf_spectrum_active_marker;
            swprintf_s(marker_text,
                       L"Marker %u: %.6f MHz  %.1f dBFS",
                       marker + 1u,
                       m_rf_spectrum_marker_frequency_hz[marker] / 1000000.0,
                       static_cast<double>(m_rf_spectrum_marker_power_dbfs[marker]));
        }
        else
        {
            swprintf_s(marker_text,
                       L"Marker %u: click the spectrum to place",
                       m_rf_spectrum_active_marker + 1u);
        }
        RfSpectrumMarkerStatusText().Text(marker_text);
        RfSpectrumMarker1Button().IsChecked(m_rf_spectrum_active_marker == 0u);
        RfSpectrumMarker2Button().IsChecked(m_rf_spectrum_active_marker == 1u);
        RfSpectrumPeakTrace().Visibility(m_rf_spectrum_peak_hold
            ? Visibility::Visible
            : Visibility::Collapsed);
    }

    void MainWindow::RenderRfSpectrum()
    {
        if (!m_rf_spectrum_controls_ready || m_rf_spectrum_count == 0u)
        {
            return;
        }

        double const vertical_range = 6.0 * m_rf_spectrum_scale_db;
        auto power_y = [&](float level)
        {
            return static_cast<float>(std::clamp(
                (m_rf_spectrum_reference_dbfs - static_cast<double>(level)) /
                    vertical_range * 300.0,
                0.0,
                300.0));
        };
        auto peak_at = [&](double frequency_hz)
        {
            double const fractional = std::clamp(
                (frequency_hz - m_rf_spectrum_source_start_hz) / m_rf_spectrum_bin_hz,
                0.0,
                static_cast<double>(m_rf_spectrum_count - 1u));
            uint32_t const lower = static_cast<uint32_t>(std::floor(fractional));
            uint32_t const upper = (std::min)(lower + 1u, m_rf_spectrum_count - 1u);
            float const blend = static_cast<float>(fractional - static_cast<double>(lower));
            return m_rf_spectrum_peak[lower] +
                blend * (m_rf_spectrum_peak[upper] - m_rf_spectrum_peak[lower]);
        };

        constexpr uint32_t display_points = 501u;
        PointCollection live_points;
        PointCollection peak_points;
        for (uint32_t point = 0u; point < display_points; ++point)
        {
            float const x = 1000.0f * static_cast<float>(point) /
                static_cast<float>(display_points - 1u);
            double const frequency_hz = RfSpectrumFrequencyFromX(x);
            live_points.Append(Point{ x, power_y(RfSpectrumPowerAtFrequency(frequency_hz)) });
            peak_points.Append(Point{ x, power_y(peak_at(frequency_hz)) });
        }
        RfSpectrumLiveTrace().Points(live_points);
        RfSpectrumPeakTrace().Points(peak_points);

        std::array<Line, 2> marker_lines{ RfSpectrumMarker1Line(), RfSpectrumMarker2Line() };
        for (uint32_t marker = 0u; marker < 2u; ++marker)
        {
            double const x = RfSpectrumXFromFrequency(m_rf_spectrum_marker_frequency_hz[marker]);
            bool const visible = m_rf_spectrum_marker_valid[marker] && x >= 0.0 && x <= 1000.0;
            marker_lines[marker].Visibility(visible ? Visibility::Visible : Visibility::Collapsed);
            if (visible)
            {
                marker_lines[marker].X1(x);
                marker_lines[marker].X2(x);
            }
        }

        std::vector<float> visible_levels;
        uint32_t const first = static_cast<uint32_t>(std::clamp(
            std::floor((RfSpectrumFrequencyFromX(0.0) - m_rf_spectrum_source_start_hz) /
                       m_rf_spectrum_bin_hz),
            0.0,
            static_cast<double>(m_rf_spectrum_count - 1u)));
        uint32_t const last = static_cast<uint32_t>(std::clamp(
            std::ceil((RfSpectrumFrequencyFromX(1000.0) - m_rf_spectrum_source_start_hz) /
                      m_rf_spectrum_bin_hz),
            0.0,
            static_cast<double>(m_rf_spectrum_count - 1u)));
        visible_levels.reserve(last >= first ? last - first + 1u : 0u);
        for (uint32_t bin = first; bin <= last && bin < m_rf_spectrum_count; ++bin)
        {
            visible_levels.push_back(m_rf_spectrum_average[bin]);
        }
        if (!visible_levels.empty())
        {
            size_t const percentile = visible_levels.size() / 5u;
            std::nth_element(visible_levels.begin(),
                             visible_levels.begin() + percentile,
                             visible_levels.end());
            wchar_t noise[48]{};
            swprintf_s(noise,
                       L"Noise floor (p20): %.1f dBFS",
                       static_cast<double>(visible_levels[percentile]));
            RfSpectrumNoiseFloorText().Text(noise);
        }
        UpdateRfSpectrumReadouts();
    }

    void MainWindow::AppendRfSpectrumWaterfall()
    {
        if (!m_rf_spectrum_controls_ready || m_rf_spectrum_count == 0u ||
            !m_rf_workspace_active)
        {
            return;
        }

        auto waterfall = RfSpectrumWaterfallCanvas().Children();
        constexpr uint32_t columns = 128u;
        constexpr uint32_t rows = 50u;
        constexpr double row_height = 3.0;
        while (waterfall.Size() >= columns * rows)
        {
            for (uint32_t column = 0u; column < columns; ++column)
            {
                waterfall.RemoveAt(0u);
            }
        }
        for (uint32_t index = 0u; index < waterfall.Size(); ++index)
        {
            auto const element = waterfall.GetAt(index);
            Canvas::SetTop(element, Canvas::GetTop(element) + row_height);
        }

        double const lower = m_rf_spectrum_reference_dbfs - 6.0 * m_rf_spectrum_scale_db;
        double const range = (std::max)(m_rf_spectrum_reference_dbfs - lower, 1.0);
        for (uint32_t column = 0u; column < columns; ++column)
        {
            double const x = 1000.0 * (static_cast<double>(column) + 0.5) /
                static_cast<double>(columns);
            float const level = RfSpectrumPowerAtFrequency(RfSpectrumFrequencyFromX(x));
            float const intensity = static_cast<float>(std::clamp(
                (static_cast<double>(level) - lower) / range,
                0.0,
                1.0));
            uint8_t red = intensity > 0.70f
                ? static_cast<uint8_t>(255.0f * (intensity - 0.70f) / 0.30f)
                : 0u;
            uint8_t green = static_cast<uint8_t>(24.0f + 220.0f * intensity);
            uint8_t blue = static_cast<uint8_t>(42.0f +
                205.0f * (std::min)(intensity * 1.8f, 1.0f));
            Microsoft::UI::Xaml::Shapes::Rectangle cell;
            cell.Width((1000.0 / static_cast<double>(columns)) + 0.5);
            cell.Height(row_height + 0.2);
            cell.Fill(SolidColorBrush(ColorHelper::FromArgb(255u, red, green, blue)));
            Canvas::SetLeft(cell, 1000.0 * static_cast<double>(column) /
                                  static_cast<double>(columns));
            Canvas::SetTop(cell, 0.0);
            waterfall.Append(cell);
        }
    }

    float MainWindow::RfNullPowerAtTime(double time_ms, bool average) const noexcept
    {
        if (m_rf_null_history_count == 0u || m_rf_null_sample_rate_hz == 0u)
        {
            return -120.0f;
        }
        bool const use_frame = m_rf_null_time_div_us == 0.0 ||
                               m_rf_null_time_div_us >= 10000.0;
        size_t const frames = average
            ? (std::min)(m_rf_null_history_count,
                         static_cast<size_t>(m_rf_null_average_factor))
            : 1u;
        double power_sum = 0.0;
        size_t used = 0u;
        for (size_t age = 0u; age < frames; ++age)
        {
            size_t const history_index =
                (m_rf_null_history_write + rf_null_history_capacity - 1u - age) %
                rf_null_history_capacity;
            float level = -120.0f;
            bool valid = false;
            if (use_frame && m_rf_null_frame_bin_count != 0u &&
                m_rf_null_frame_bin_samples > 0.0f && time_ms >= 0.0)
            {
                double const sample = time_ms * static_cast<double>(m_rf_null_sample_rate_hz) /
                                      1000.0;
                double const bin = sample / static_cast<double>(m_rf_null_frame_bin_samples);
                if (bin >= 0.0 && bin <= static_cast<double>(m_rf_null_frame_bin_count - 1u))
                {
                    uint32_t const lower = static_cast<uint32_t>(std::floor(bin));
                    uint32_t const upper = (std::min)(lower + 1u, m_rf_null_frame_bin_count - 1u);
                    float const blend = static_cast<float>(bin - static_cast<double>(lower));
                    level = m_rf_null_frame_history[history_index][lower] +
                        blend * (m_rf_null_frame_history[history_index][upper] -
                                 m_rf_null_frame_history[history_index][lower]);
                    valid = true;
                }
            }
            else if (!use_frame && m_rf_null_focus_count != 0u &&
                     m_rf_null_sample_period_ns > 0.0f)
            {
                double const sample = time_ms * 1000000.0 /
                                      static_cast<double>(m_rf_null_sample_period_ns) -
                                      static_cast<double>(m_rf_null_start_offset_samples);
                if (sample >= 0.0 && sample <= static_cast<double>(m_rf_null_focus_count - 1u))
                {
                    uint32_t const center = static_cast<uint32_t>(std::llround(sample));
                    uint32_t const radius = m_rf_null_filtered_source ? 3u : 0u;
                    uint32_t const begin = center > radius ? center - radius : 0u;
                    uint32_t const end = (std::min)(m_rf_null_focus_count - 1u, center + radius);
                    double local_power = 0.0;
                    for (uint32_t point = begin; point <= end; ++point)
                    {
                        local_power += std::pow(10.0,
                            static_cast<double>(m_rf_null_focus_history[history_index][point]) / 10.0);
                    }
                    local_power /= static_cast<double>(end - begin + 1u);
                    level = local_power > 1.0e-12
                        ? static_cast<float>(10.0 * std::log10(local_power))
                        : -120.0f;
                    valid = true;
                }
            }
            if (valid)
            {
                power_sum += std::pow(10.0, static_cast<double>(level) / 10.0);
                ++used;
            }
        }
        return used != 0u && power_sum > 1.0e-12
            ? static_cast<float>(10.0 * std::log10(power_sum / static_cast<double>(used)))
            : -120.0f;
    }

    void MainWindow::UpdateRfNullScope(dab::measurement::MeasurementFrame const& frame)
    {
        if (!m_rf_null_controls_ready || frame.null_scope_sample_count == 0u ||
            frame.null_scope_sample_period_ns <= 0.0f)
        {
            return;
        }
        if (m_rf_null_frozen && !m_rf_null_single_armed)
        {
            return;
        }
        uint64_t const holdoff_ns = static_cast<uint64_t>(m_rf_null_holdoff_ms * 1000000.0);
        if (m_rf_null_last_accept_timestamp_ns != 0u &&
            frame.timestamp_ns > m_rf_null_last_accept_timestamp_ns &&
            frame.timestamp_ns - m_rf_null_last_accept_timestamp_ns < holdoff_ns)
        {
            return;
        }

        size_t const slot = m_rf_null_history_write;
        uint32_t const focus_count = (std::min)(
            frame.null_scope_sample_count,
            static_cast<uint32_t>(DAB_RECEIVE_NULL_SCOPE_SAMPLE_CAPACITY));
        uint32_t const frame_count = (std::min)(
            frame.null_scope_frame_bin_count,
            static_cast<uint32_t>(DAB_RECEIVE_NULL_SCOPE_FRAME_BIN_COUNT));
        std::copy_n(frame.null_scope_power_dbfs.begin(),
                    focus_count,
                    m_rf_null_focus_history[slot].begin());
        if (frame_count != 0u)
        {
            std::copy_n(frame.null_scope_frame_power_dbfs.begin(),
                        frame_count,
                        m_rf_null_frame_history[slot].begin());
        }
        std::copy_n(frame.null_scope_iq_interleaved.begin(),
                    2u * focus_count,
                    m_rf_null_latest_iq.begin());
        m_rf_null_timing_history[slot] = static_cast<int32_t>(frame.null_scope_start_index) - 6144;
        m_rf_null_frame_start_history[slot] = frame.frame_start_sample;
        m_rf_null_history_write = (slot + 1u) % rf_null_history_capacity;
        m_rf_null_history_count = (std::min)(m_rf_null_history_count + 1u,
                                              rf_null_history_capacity);

        m_rf_null_focus_count = focus_count;
        m_rf_null_frame_bin_count = frame_count;
        m_rf_null_frame_sample_count = frame.null_scope_frame_sample_count;
        m_rf_null_duration_samples = frame.null_scope_duration_samples;
        m_rf_null_start_index = frame.null_scope_start_index;
        m_rf_null_end_index = frame.null_scope_end_index;
        m_rf_null_start_offset_samples = frame.null_scope_start_offset_samples;
        m_rf_null_sample_period_ns = frame.null_scope_sample_period_ns;
        m_rf_null_frame_bin_samples = frame.null_scope_frame_bin_samples;
        m_rf_null_threshold_dbfs = frame.null_scope_threshold_dbfs;
        m_rf_null_depth_db = frame.null_scope_depth_db;
        m_rf_null_confidence = frame.null_scope_confidence;
        m_rf_null_energy_dbfs = frame.null_scope_energy_dbfs;
        m_rf_null_sequence = frame.sequence;
        m_rf_null_frequency_hz = frame.frequency_hz;
        m_rf_null_sample_rate_hz = frame.sample_rate_hz;
        m_rf_null_last_accept_timestamp_ns = frame.timestamp_ns;
        m_rf_null_locked = true;

        if (m_rf_null_history_count == 1u)
        {
            m_rf_null_cursor_ms[0] = 0.0;
            m_rf_null_cursor_ms[1] = static_cast<double>(m_rf_null_duration_samples) *
                static_cast<double>(m_rf_null_sample_period_ns) / 1000000.0;
        }
        if (m_rf_null_single_armed)
        {
            m_rf_null_single_armed = false;
            m_rf_null_frozen = true;
            RfNullFreezeButton().IsChecked(true);
            RfNullControlStatusText().Text(L"Single capture complete");
            RfNullLiveText().Text(L"FROZEN");
        }
        else
        {
            RfNullControlStatusText().Text(L"Live acquisition");
            RfNullLiveText().Text(L"LIVE");
        }
        RenderRfNullScope();
        UpdateRfNullScopeReadouts();
    }

    void MainWindow::RenderRfNullScope()
    {
        if (!m_rf_null_controls_ready || m_rf_null_history_count == 0u)
        {
            return;
        }
        constexpr double plot_left = 65.0;
        constexpr double plot_width = 905.0;
        constexpr double plot_top = 25.0;
        constexpr double plot_height = 310.0;
        double span_ms;
        double start_ms;
        if (m_rf_null_time_div_us == 0.0)
        {
            span_ms = m_rf_null_sample_rate_hz != 0u
                ? 1000.0 * static_cast<double>(m_rf_null_frame_sample_count) /
                    static_cast<double>(m_rf_null_sample_rate_hz)
                : 96.0;
            start_ms = 0.0;
        }
        else
        {
            span_ms = 7.0 * m_rf_null_time_div_us / 1000.0;
            start_ms = m_rf_null_time_div_us >= 10000.0
                ? 0.0
                : -span_ms * m_rf_null_trigger_position_percent / 100.0;
        }
        span_ms = (std::max)(span_ms, 0.001);
        double const end_ms = start_ms + span_ms;
        double const lower_dbfs = -5.0 * m_rf_null_vertical_db_div;
        auto level_y = [&](float level)
        {
            return plot_top + std::clamp(
                -static_cast<double>(level) / (std::max)(-lower_dbfs, 1.0) * plot_height,
                0.0,
                plot_height);
        };
        auto time_x = [&](double time_ms)
        {
            return plot_left + (time_ms - start_ms) / span_ms * plot_width;
        };

        size_t const average_frames = (std::min)(
            m_rf_null_history_count,
            static_cast<size_t>(m_rf_null_average_factor));
        auto one_frame_level = [&](size_t history_index, double time_ms)
        {
            bool const use_frame = m_rf_null_time_div_us == 0.0 ||
                                   m_rf_null_time_div_us >= 10000.0;
            if (use_frame)
            {
                if (m_rf_null_frame_bin_count == 0u || m_rf_null_frame_bin_samples <= 0.0f ||
                    time_ms < 0.0)
                {
                    return -120.0f;
                }
                double const source_sample = time_ms * m_rf_null_sample_rate_hz / 1000.0;
                double const source_bin = source_sample / m_rf_null_frame_bin_samples;
                if (source_bin < 0.0 || source_bin > m_rf_null_frame_bin_count - 1u)
                {
                    return -120.0f;
                }
                uint32_t const center = static_cast<uint32_t>(std::llround(source_bin));
                uint32_t const radius = m_rf_null_filtered_source ? 1u : 0u;
                uint32_t const begin = center > radius ? center - radius : 0u;
                uint32_t const last = (std::min)(m_rf_null_frame_bin_count - 1u, center + radius);
                double power = 0.0;
                for (uint32_t bin = begin; bin <= last; ++bin)
                {
                    power += std::pow(10.0,
                        static_cast<double>(m_rf_null_frame_history[history_index][bin]) / 10.0);
                }
                return power > 1.0e-12
                    ? static_cast<float>(10.0 * std::log10(power / (last - begin + 1u)))
                    : -120.0f;
            }
            if (m_rf_null_focus_count == 0u || m_rf_null_sample_period_ns <= 0.0f)
            {
                return -120.0f;
            }
            double const source_sample = time_ms * 1000000.0 / m_rf_null_sample_period_ns -
                                         m_rf_null_start_offset_samples;
            if (source_sample < 0.0 || source_sample > m_rf_null_focus_count - 1u)
            {
                return -120.0f;
            }
            uint32_t const center = static_cast<uint32_t>(std::llround(source_sample));
            uint32_t const radius = m_rf_null_filtered_source ? 3u : 0u;
            uint32_t const begin = center > radius ? center - radius : 0u;
            uint32_t const last = (std::min)(m_rf_null_focus_count - 1u, center + radius);
            double power = 0.0;
            for (uint32_t sample = begin; sample <= last; ++sample)
            {
                power += std::pow(10.0,
                    static_cast<double>(m_rf_null_focus_history[history_index][sample]) / 10.0);
            }
            return power > 1.0e-12
                ? static_cast<float>(10.0 * std::log10(power / (last - begin + 1u)))
                : -120.0f;
        };

        PointCollection live_points;
        PointCollection average_points;
        PointCollection minimum_points;
        PointCollection maximum_points;
        PointCollection persistence_live;
        PointCollection persistence_average;
        PointCollection persistence_minimum;
        PointCollection persistence_maximum;
        constexpr uint32_t display_points = 501u;
        size_t const latest = (m_rf_null_history_write + rf_null_history_capacity - 1u) %
                              rf_null_history_capacity;
        for (uint32_t point = 0u; point < display_points; ++point)
        {
            double const fraction = static_cast<double>(point) /
                                    static_cast<double>(display_points - 1u);
            double const time_ms = start_ms + fraction * span_ms;
            float const live = one_frame_level(latest, time_ms);
            double average_power = 0.0;
            float minimum = 120.0f;
            float maximum = -120.0f;
            for (size_t age = 0u; age < average_frames; ++age)
            {
                size_t const history_index =
                    (m_rf_null_history_write + rf_null_history_capacity - 1u - age) %
                    rf_null_history_capacity;
                float const level = one_frame_level(history_index, time_ms);
                average_power += std::pow(10.0, static_cast<double>(level) / 10.0);
                minimum = (std::min)(minimum, level);
                maximum = (std::max)(maximum, level);
            }
            float const average = average_power > 1.0e-12
                ? static_cast<float>(10.0 * std::log10(average_power /
                                                       static_cast<double>(average_frames)))
                : -120.0f;
            float const x = static_cast<float>(plot_left + fraction * plot_width);
            live_points.Append(Point{ x, static_cast<float>(level_y(live)) });
            average_points.Append(Point{ x, static_cast<float>(level_y(average)) });
            minimum_points.Append(Point{ x, static_cast<float>(level_y(minimum)) });
            maximum_points.Append(Point{ x, static_cast<float>(level_y(maximum)) });
            float const persistence_x = 1000.0f * static_cast<float>(fraction);
            auto persistence_y = [&](float level)
            {
                return static_cast<float>(std::clamp(
                    -static_cast<double>(level) / (std::max)(-lower_dbfs, 1.0) * 100.0,
                    0.0,
                    100.0));
            };
            persistence_live.Append(Point{ persistence_x, persistence_y(live) });
            persistence_average.Append(Point{ persistence_x, persistence_y(average) });
            persistence_minimum.Append(Point{ persistence_x, persistence_y(minimum) });
            persistence_maximum.Append(Point{ persistence_x, persistence_y(maximum) });
        }
        RfNullLiveTrace().Points(live_points);
        RfNullAverageTrace().Points(average_points);
        RfNullMinimumTrace().Points(minimum_points);
        RfNullMaximumTrace().Points(maximum_points);
        RfNullPersistenceNewestTrace().Points(persistence_live);
        RfNullPersistenceAverageTrace().Points(persistence_average);
        RfNullPersistenceMinTrace().Points(persistence_minimum);
        RfNullPersistenceMaxTrace().Points(persistence_maximum);

        Visibility const persistence = m_rf_null_persistence_enabled
            ? Visibility::Visible
            : Visibility::Collapsed;
        RfNullMinimumTrace().Visibility(persistence);
        RfNullMaximumTrace().Visibility(persistence);
        RfNullPersistenceMinTrace().Visibility(persistence);
        RfNullPersistenceMaxTrace().Visibility(persistence);
        RfNullAverageTrace().Visibility(m_rf_null_average_factor > 1u
            ? Visibility::Visible
            : Visibility::Collapsed);
        RfNullPersistenceAverageTrace().Visibility(m_rf_null_average_factor > 1u
            ? Visibility::Visible
            : Visibility::Collapsed);

        double const threshold_y = level_y(m_rf_null_threshold_dbfs);
        RfNullThresholdLine().Y1(threshold_y);
        RfNullThresholdLine().Y2(threshold_y);
        Canvas::SetTop(RfNullThresholdText(), std::clamp(threshold_y + 2.0, 25.0, 320.0));
        RfNullThresholdText().Text(L"threshold " + format_float(m_rf_null_threshold_dbfs, L" dBFS", 1));

        double const null_end_ms = static_cast<double>(m_rf_null_duration_samples) *
                                   m_rf_null_sample_period_ns / 1000000.0;
        auto place_boundary = [&](Line const& line, TextBlock const& label, double time_ms)
        {
            bool const visible = time_ms >= start_ms && time_ms <= end_ms;
            line.Visibility(visible ? Visibility::Visible : Visibility::Collapsed);
            label.Visibility(visible ? Visibility::Visible : Visibility::Collapsed);
            if (visible)
            {
                double const x = time_x(time_ms);
                line.X1(x);
                line.X2(x);
                Canvas::SetLeft(label, std::clamp(x - 42.0, plot_left, plot_left + plot_width - 90.0));
            }
        };
        place_boundary(RfNullStartLine(), RfNullStartText(), 0.0);
        place_boundary(RfNullEndLine(), RfNullEndText(), null_end_ms);

        std::array<Line, 2> const cursors{ RfNullCursor1Line(), RfNullCursor2Line() };
        for (uint32_t cursor = 0u; cursor < 2u; ++cursor)
        {
            bool const visible = m_rf_null_cursors_enabled &&
                m_rf_null_cursor_ms[cursor] >= start_ms && m_rf_null_cursor_ms[cursor] <= end_ms;
            cursors[cursor].Visibility(visible ? Visibility::Visible : Visibility::Collapsed);
            if (visible)
            {
                double const x = time_x(m_rf_null_cursor_ms[cursor]);
                cursors[cursor].X1(x);
                cursors[cursor].X2(x);
            }
        }
    }

    void MainWindow::UpdateRfNullScopeReadouts()
    {
        if (!m_rf_null_controls_ready)
        {
            return;
        }
        double span_ms;
        double start_ms;
        if (m_rf_null_time_div_us == 0.0)
        {
            span_ms = m_rf_null_sample_rate_hz != 0u
                ? 1000.0 * m_rf_null_frame_sample_count / m_rf_null_sample_rate_hz
                : 96.0;
            start_ms = 0.0;
        }
        else
        {
            span_ms = 7.0 * m_rf_null_time_div_us / 1000.0;
            start_ms = m_rf_null_time_div_us >= 10000.0
                ? 0.0
                : -span_ms * m_rf_null_trigger_position_percent / 100.0;
        }
        double const end_ms = start_ms + span_ms;
        wchar_t view[80]{};
        swprintf_s(view, L"%+.3f ms ... %+.3f ms", start_ms, end_ms);
        RfNullViewText().Text(view);
        RfNullAxisLeftText().Text(format_signed_float(static_cast<float>(start_ms), L" ms", 3));
        RfNullAxisRightText().Text(format_signed_float(static_cast<float>(end_ms), L" ms", 3));
        std::array<TextBlock, 6> const y_labels{
            RfNullY0Text(), RfNullY1Text(), RfNullY2Text(),
            RfNullY3Text(), RfNullY4Text(), RfNullY5Text()
        };
        for (uint32_t division = 0u; division < y_labels.size(); ++division)
        {
            wchar_t level[32]{};
            swprintf_s(level,
                       division == 0u ? L"0 dBFS" : L"%.0f",
                       -static_cast<double>(division) * m_rf_null_vertical_db_div);
            y_labels[division].Text(level);
        }
        if (m_rf_null_history_count == 0u)
        {
            return;
        }

        hstring channel{ L"custom" };
        if (auto const selected = ChannelComboBox().SelectedItem().try_as<ComboBoxItem>())
        {
            channel = unbox_value<hstring>(selected.Content());
        }
        wchar_t context[160]{};
        swprintf_s(context,
                   L"Channel %s \x00B7 %.6f MHz \x00B7 %.3f MS/s \x00B7 Frame #%llu",
                   channel.c_str(),
                   static_cast<double>(m_rf_null_frequency_hz) / 1000000.0,
                   static_cast<double>(m_rf_null_sample_rate_hz) / 1000000.0,
                   static_cast<unsigned long long>(m_rf_null_sequence));
        RfNullContextText().Text(context);
        RfNullDurationSamplesText().Text(to_hstring(m_rf_null_duration_samples) + L" samples");
        RfNullDurationTimeText().Text(format_float(
            static_cast<float>(m_rf_null_duration_samples) * m_rf_null_sample_period_ns / 1000000.0f,
            L" ms",
            3));
        RfNullDepthText().Text(format_float(m_rf_null_depth_db, L" dB", 1));
        RfNullConfidenceText().Text(format_float(m_rf_null_confidence * 100.0f, L" %", 1));

        float const frame_period_ms = m_rf_null_frame_sample_count != 0u &&
                                      m_rf_null_sample_rate_hz != 0u
            ? 1000.0f * static_cast<float>(m_rf_null_frame_sample_count) /
                static_cast<float>(m_rf_null_sample_rate_hz)
            : 0.0f;
        RfNullFramePeriodText().Text(frame_period_ms > 0.0f
            ? format_float(frame_period_ms, L" ms", 3)
            : hstring{ L"Collecting..." });

        double mean = 0.0;
        for (size_t age = 0u; age < m_rf_null_history_count; ++age)
        {
            size_t const index = (m_rf_null_history_write + rf_null_history_capacity - 1u - age) %
                                 rf_null_history_capacity;
            mean += m_rf_null_timing_history[index];
        }
        mean /= static_cast<double>(m_rf_null_history_count);
        double variance = 0.0;
        for (size_t age = 0u; age < m_rf_null_history_count; ++age)
        {
            size_t const index = (m_rf_null_history_write + rf_null_history_capacity - 1u - age) %
                                 rf_null_history_capacity;
            double const error = m_rf_null_timing_history[index] - mean;
            variance += error * error;
        }
        double const jitter = m_rf_null_history_count > 1u
            ? std::sqrt(variance / static_cast<double>(m_rf_null_history_count - 1u))
            : 0.0;
        RfNullJitterText().Text(L"\x00B1" + format_float(static_cast<float>(jitter), L" samples", 1));
        RfNullTiiStatusText().Text(m_rf_null_confidence > 0.5f
            ? L"Energy observed; ID not decoded"
            : L"No reliable null energy estimate");
        RfNullTiiLevelText().Text(format_float(m_rf_null_energy_dbfs, L" dBFS", 1));
        RfNullPersistenceStatusText().Text(
            to_hstring(m_rf_null_history_count) + L"/32 frames \x00B7 linear-power average \x00B7 min/max envelope");

        float const m1_power = RfNullPowerAtTime(m_rf_null_cursor_ms[0], false);
        float const m2_power = RfNullPowerAtTime(m_rf_null_cursor_ms[1], false);
        wchar_t cursors[220]{};
        swprintf_s(cursors,
                   L"M1: %.3f ms / %.1f dBFS    M2: %.3f ms / %.1f dBFS    \x0394t: %.3f ms",
                   m_rf_null_cursor_ms[0],
                   static_cast<double>(m1_power),
                   m_rf_null_cursor_ms[1],
                   static_cast<double>(m2_power),
                   m_rf_null_cursor_ms[1] - m_rf_null_cursor_ms[0]);
        RfNullCursorReadoutText().Text(cursors);
    }

    void MainWindow::ResetRfNullScope()
    {
        m_rf_null_history_write = 0u;
        m_rf_null_history_count = 0u;
        m_rf_null_focus_count = 0u;
        m_rf_null_frame_bin_count = 0u;
        m_rf_null_frame_sample_count = 0u;
        m_rf_null_duration_samples = 0u;
        m_rf_null_sequence = 0u;
        m_rf_null_last_accept_timestamp_ns = 0u;
        m_rf_null_locked = false;
        m_rf_null_frozen = false;
        m_rf_null_single_armed = false;
        if (m_rf_null_controls_ready)
        {
            RfNullControlStatusText().Text(L"Awaiting frame trigger");
            RfNullPersistenceStatusText().Text(L"Awaiting frames");
            RfNullLiveText().Text(L"WAITING");
            RfNullCaptureStatusText().Text(L"");
            RfNullFreezeButton().IsChecked(false);
        }
    }

    void MainWindow::UpdateReceiverSpectrumFrequencyScale(
        double start_hz,
        double stop_hz)
    {
        if (!std::isfinite(start_hz) ||
            !std::isfinite(stop_hz) ||
            stop_hz <= start_hz)
        {
            return;
        }

        std::array<TextBlock, 5u> const labels{
            ReceiverSpectrumFrequency0Text(),
            ReceiverSpectrumFrequency1Text(),
            ReceiverSpectrumFrequency2Text(),
            ReceiverSpectrumFrequency3Text(),
            ReceiverSpectrumFrequency4Text()
        };
        for (size_t index = 0u; index < labels.size(); ++index)
        {
            double const fraction =
                static_cast<double>(index) /
                static_cast<double>(labels.size() - 1u);
            double const frequency_mhz =
                (start_hz + fraction * (stop_hz - start_hz)) / 1000000.0;
            wchar_t text[32]{};
            swprintf_s(text, L"%.3f", frequency_mhz);
            labels[index].Text(text);
        }
    }

    void MainWindow::UpdatePlots(dab::measurement::MeasurementFrame const& frame)
    {
        if (m_metrics_stale)
        {
            return;
        }
        DataModeText().Text(L"HIGH-RATE MEASUREMENTS");
        DataSeamText().Text(L"measurement " + to_hstring(frame.sequence));
        SampleCounterText().Text(L"measurement " + to_hstring(frame.sequence));

        if (frame.null_scope_sample_count != 0u)
        {
            UpdateRfNullScope(frame);
        }

        if (frame.spectrum_bin_count != 0u)
        {
            UpdateRfSpectrum(frame);
            double const spectrum_bin_hz = frame.spectrum_bin_hz > 0.0f
                ? static_cast<double>(frame.spectrum_bin_hz)
                : (frame.sample_rate_hz > 0u
                    ? static_cast<double>(frame.sample_rate_hz) /
                        static_cast<double>(frame.spectrum_bin_count)
                    : 1000.0);
            double const spectrum_start_hz =
                static_cast<double>(frame.frequency_hz) +
                static_cast<double>(frame.spectrum_start_hz);
            double const spectrum_stop_hz =
                spectrum_start_hz +
                spectrum_bin_hz *
                    static_cast<double>(frame.spectrum_bin_count - 1u);
            UpdateReceiverSpectrumFrequencyScale(
                spectrum_start_hz,
                spectrum_stop_hz);

            wchar_t rbw_text[48]{};
            swprintf_s(
                rbw_text,
                L"RBW %.3f kHz",
                spectrum_bin_hz / 1000.0);
            ReceiverSpectrumRbwText().Text(rbw_text);
            if (frame.sample_rate_hz > 0u)
            {
                wchar_t sample_rate_text[48]{};
                swprintf_s(
                    sample_rate_text,
                    L"%.3f MS/s",
                    static_cast<double>(frame.sample_rate_hz) / 1000000.0);
                SampleRateText().Text(sample_rate_text);
            }
            if (frame.frequency_hz > 0u)
            {
                wchar_t center_text[48]{};
                swprintf_s(
                    center_text,
                    L"%.3f MHz",
                    static_cast<double>(frame.frequency_hz) / 1000000.0);
                SpectrumFrequencyText().Text(center_text);
            }

            PointCollection spectrum_points;
            constexpr uint32_t display_points = 181u;
            constexpr float plot_left = 58.0f;
            constexpr float plot_top = 16.0f;
            constexpr float plot_width = 826.0f;
            constexpr float plot_height = 210.0f;
            for (uint32_t point = 0u; point < display_points; ++point)
            {
                uint32_t const begin = (point * frame.spectrum_bin_count) / display_points;
                uint32_t const end = (std::max)(begin + 1u,
                    ((point + 1u) * frame.spectrum_bin_count) / display_points);
                float level = -120.0f;
                for (uint32_t bin = begin; bin < (std::min)(end, frame.spectrum_bin_count); ++bin)
                {
                    level = (std::max)(level, frame.spectrum_dbfs[bin]);
                }
                float const x = plot_left +
                                plot_width * static_cast<float>(point) /
                                static_cast<float>(display_points - 1u);
                float const clamped_level =
                    std::clamp(level, -120.0f, -20.0f);
                float const y = plot_top +
                    ((-20.0f - clamped_level) / 100.0f) * plot_height;
                spectrum_points.Append(Point{ x, y });
            }
            SpectrumTrace().Points(spectrum_points);

            auto waterfall = WaterfallCanvas().Children();
            constexpr uint32_t waterfall_bins = 96u;
            constexpr uint32_t waterfall_rows = 24u;
            while (waterfall.Size() >= waterfall_bins * waterfall_rows)
            {
                for (uint32_t bin = 0u; bin < waterfall_bins; ++bin)
                {
                    waterfall.RemoveAt(0u);
                }
            }
            for (uint32_t i = 0u; i < waterfall.Size(); ++i)
            {
                auto element = waterfall.GetAt(i);
                Canvas::SetTop(element, Canvas::GetTop(element) + 5.0);
            }
            for (uint32_t column = 0u; column < waterfall_bins; ++column)
            {
                uint32_t const begin = (column * frame.spectrum_bin_count) / waterfall_bins;
                uint32_t const end = (std::max)(begin + 1u,
                    ((column + 1u) * frame.spectrum_bin_count) / waterfall_bins);
                float level = -120.0f;
                for (uint32_t bin = begin; bin < (std::min)(end, frame.spectrum_bin_count); ++bin)
                {
                    level = (std::max)(level, frame.spectrum_dbfs[bin]);
                }
                float const intensity = std::clamp((level + 115.0f) / 75.0f, 0.0f, 1.0f);
                uint8_t red = 0u;
                uint8_t green = static_cast<uint8_t>(30.0f + (205.0f * intensity));
                uint8_t blue = static_cast<uint8_t>(55.0f + (200.0f * (std::min)(intensity * 2.0f, 1.0f)));
                if (intensity > 0.72f)
                {
                    red = static_cast<uint8_t>(255.0f * ((intensity - 0.72f) / 0.28f));
                }

                Microsoft::UI::Xaml::Shapes::Rectangle cell;
                cell.Width((900.0 / static_cast<double>(waterfall_bins)) + 0.5);
                cell.Height(5.25);
                cell.Fill(SolidColorBrush(ColorHelper::FromArgb(255u, red, green, blue)));
                Canvas::SetLeft(cell, 900.0 * static_cast<double>(column) /
                                      static_cast<double>(waterfall_bins));
                Canvas::SetTop(cell, 0.0);
                waterfall.Append(cell);
            }
        }

        auto constellation = ConstellationPointCanvas().Children();
        constellation.Clear();
        auto const dot_brush = SolidColorBrush(ColorHelper::FromArgb(185u, 84u, 226u, 255u));
        uint32_t const constellation_points = (std::min)(
            frame.constellation_point_count,
            256u);
        for (uint32_t point = 0u; point < constellation_points; ++point)
        {
            uint32_t const source = (point * frame.constellation_point_count) /
                                    constellation_points;
            constexpr double axis_min = -1.2;
            constexpr double axis_max = 1.2;
            constexpr double plot_left = 30.0;
            constexpr double plot_top = 20.0;
            constexpr double plot_size = 230.0;
            double const normalized_i = std::clamp(
                static_cast<double>(frame.constellation_i[source]),
                axis_min,
                axis_max);
            double const normalized_q = std::clamp(
                static_cast<double>(frame.constellation_q[source]),
                axis_min,
                axis_max);
            double const x = plot_left +
                (normalized_i - axis_min) / (axis_max - axis_min) * plot_size;
            double const y = plot_top +
                (axis_max - normalized_q) / (axis_max - axis_min) * plot_size;
            Microsoft::UI::Xaml::Shapes::Ellipse dot;
            dot.Width(3.0);
            dot.Height(3.0);
            dot.Fill(dot_brush);
            Canvas::SetLeft(dot, x - 1.5);
            Canvas::SetTop(dot, y - 1.5);
            constellation.Append(dot);
        }

        if (frame.cir_bin_count != 0u)
        {
            UpdateCirPlot(frame);
        }

        if (frame.carrier_mer_count != 0u)
        {
            PointCollection mer_points;
            constexpr uint32_t display_points = 193u;
            double mer_sum = 0.0;
            float minimum_mer = frame.carrier_mer_db[0u];
            for (uint32_t carrier = 0u; carrier < frame.carrier_mer_count; ++carrier)
            {
                mer_sum += frame.carrier_mer_db[carrier];
                minimum_mer = (std::min)(minimum_mer, frame.carrier_mer_db[carrier]);
            }
            for (uint32_t point = 0u; point < display_points; ++point)
            {
                uint32_t const begin = (point * frame.carrier_mer_count) / display_points;
                uint32_t const end = (std::max)(begin + 1u,
                    ((point + 1u) * frame.carrier_mer_count) / display_points);
                double sum = 0.0;
                uint32_t count = 0u;
                for (uint32_t carrier = begin; carrier < (std::min)(end, frame.carrier_mer_count); ++carrier)
                {
                    sum += frame.carrier_mer_db[carrier];
                    ++count;
                }
                float const mer = count != 0u ? static_cast<float>(sum / count) : -20.0f;
                float const x = 800.0f * static_cast<float>(point) /
                                static_cast<float>(display_points - 1u);
                float const y = std::clamp(175.0f - (mer * 3.0f), 12.0f, 180.0f);
                mer_points.Append(Point{ x, y });
            }
            CarrierMerTrace().Points(mer_points);
            float const average_mer = static_cast<float>(mer_sum /
                static_cast<double>(frame.carrier_mer_count));
            MerValueText().Text(format_float(average_mer, L" dB"));
            m_latest_mer_db = average_mer;
            m_latest_mer_valid = true;
            RfMerValueText().Text(format_float(average_mer, L" dB"));

            wchar_t mer_status[112]{};
            swprintf_s(mer_status,
                       L"Carrier −768 … +768 · avg %.1f dB · min %.1f dB",
                       static_cast<double>(average_mer),
                       static_cast<double>(minimum_mer));
            CarrierMerStatusText().Text(mer_status);
        }
        UpdateRfChannel(frame);
    }

    void MainWindow::AddTimelineEvent(hstring const& message)
    {
        if (m_replay_applying_record)
        {
            return;
        }
        m_session_recorder.RecordEvent(0x10000u, to_string(message));
        uint64_t const elapsed_ms = m_tick * 250ull;
        wchar_t prefix[32]{};
        swprintf_s(prefix,
                   L"%02llu:%02llu.%03llu  ·  ",
                   static_cast<unsigned long long>((elapsed_ms / 60000ull) % 60ull),
                   static_cast<unsigned long long>((elapsed_ms / 1000ull) % 60ull),
                   static_cast<unsigned long long>(elapsed_ms % 1000ull));

        auto items = TimelineList().Items();
        items.Append(box_value(hstring{ prefix } + message));
        while (items.Size() > 18u)
        {
            items.RemoveAt(0u);
        }
        TimelineList().ScrollIntoView(items.GetAt(items.Size() - 1u));

        if (DabEventsList())
        {
            auto dab_items = DabEventsList().Items();
            dab_items.Append(box_value(hstring{ prefix } + message));
            while (dab_items.Size() > 200u)
            {
                dab_items.RemoveAt(0u);
            }
            DabEventsList().ScrollIntoView(dab_items.GetAt(dab_items.Size() - 1u));
        }
    }
}
