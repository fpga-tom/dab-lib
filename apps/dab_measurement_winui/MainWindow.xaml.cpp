#include "pch.h"
#include "MainWindow.xaml.h"

#include <limits>

#if __has_include("MainWindow.g.cpp")
#include "MainWindow.g.cpp"
#endif

using namespace winrt;
using namespace Windows::Foundation;
using namespace Windows::UI;
using namespace Microsoft::UI::Xaml;
using namespace Microsoft::UI::Xaml::Controls;
using namespace Microsoft::UI::Xaml::Media;
using namespace Microsoft::UI::Xaml::Shapes;

namespace
{
    hstring format_float(float value, wchar_t const* suffix, int precision = 1)
    {
        wchar_t buffer[64]{};
        if (precision == 3)
        {
            swprintf_s(buffer, L"%.3f%s", static_cast<double>(value), suffix);
        }
        else
        {
            swprintf_s(buffer, L"%.1f%s", static_cast<double>(value), suffix);
        }
        return hstring{ buffer };
    }

    hstring format_integer(int64_t value, wchar_t const* suffix)
    {
        wchar_t buffer[64]{};
        swprintf_s(buffer, L"%lld%s", static_cast<long long>(value), suffix);
        return hstring{ buffer };
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

    uint64_t file_time_value(FILETIME const& value)
    {
        ULARGE_INTEGER result{};
        result.LowPart = value.dwLowDateTime;
        result.HighPart = value.dwHighDateTime;
        return result.QuadPart;
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
        m_selected_service_id = 0u;
        UpdateRfContext();
    }

    void MainWindow::ReceiverWorkspace_Click(IInspectable const&, RoutedEventArgs const&)
    {
        ShowWorkspace(false);
    }

    void MainWindow::RfMeasurements_Click(IInspectable const&, RoutedEventArgs const&)
    {
        ShowWorkspace(true);
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
            ReceiverWorkspaceMain().Width(workspace_width);
        }
        if (ReceiverTimelinePanel())
        {
            ReceiverTimelinePanel().Width(workspace_width);
        }
        if (RfWorkspace())
        {
            RfWorkspace().Width(workspace_width);
        }
        if (RfOverviewContent())
        {
            RfOverviewContent().Width((std::max)(480.0, workspace_width - 36.0));
        }
    }

    void MainWindow::RfWorkspace_SizeChanged(IInspectable const&, SizeChangedEventArgs const& args)
    {
        if (RfOverviewContent() && args.NewSize().Width > 48.0f)
        {
            RfOverviewContent().Width(static_cast<double>(args.NewSize().Width) - 36.0);
        }
    }

    void MainWindow::AudioButton_Click(IInspectable const&, RoutedEventArgs const&)
    {
        AddTimelineEvent(L"Audio playback is enabled and follows the selected DAB+ service");
    }

    void MainWindow::ServicesListView_SelectionChanged(IInspectable const&,
                                                        SelectionChangedEventArgs const&)
    {
        ApplySelectedServiceComponents();
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
        AddTimelineEvent(L"Recording is not implemented in the live receiver proof of concept");
    }

    void MainWindow::MarkerButton_Click(IInspectable const&, RoutedEventArgs const&)
    {
        AddTimelineEvent(L"Marker — operator annotation");
    }

    void MainWindow::Window_Closed(IInspectable const&, WindowEventArgs const&)
    {
        m_timer.Stop();
        m_pending_receiver_restart.reset();
        m_receiver.RequestStop();
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
            HandleReceiverStatus(receiver_status);
        }
        m_receiver.JoinFinished();
        StartPendingServiceRestart();

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
        settings.agc_enabled = false;
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
            m_rf_history_write = 0u;
            m_rf_history_count = 0u;
            m_latest_mer_valid = false;
            m_rf_was_locked = false;
            m_rf_lock_started_ns = 0u;
            UpdateRfHistoryVisuals();
        }
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
            RfLiveBadgeText().Text(L"STARTING");
            SetReceiverControlsEnabled(false);
            StartButton().IsEnabled(true);
            StartButtonLabel().Text(L"Stop");
            StatusSyncText().Text(L"opening");
            break;
        case ReceiverState::running:
            RfLiveBadgeText().Text(L"● LIVE");
            SetReceiverControlsEnabled(false);
            StartButton().IsEnabled(true);
            StartButtonLabel().Text(L"Stop");
            if (StatusSyncText().Text() == L"opening")
            {
                StatusSyncText().Text(L"acquiring");
            }
            break;
        case ReceiverState::stopping:
            RfLiveBadgeText().Text(L"STOPPING");
            SetReceiverControlsEnabled(false);
            StartButton().IsEnabled(false);
            StartButtonLabel().Text(L"Stopping...");
            StatusSyncText().Text(L"stopping");
            break;
        case ReceiverState::stopped:
            m_active_service_id = 0u;
            RfLiveBadgeText().Text(L"IDLE");
            RfStatusValueText().Text(L"IDLE");
            RfStatusDetailText().Text(L"Receiver stopped");
            m_rf_was_locked = false;
            m_rf_lock_started_ns = 0u;
            SetReceiverControlsEnabled(true);
            StartButton().IsEnabled(true);
            StartButtonLabel().Text(L"Start");
            StatusSyncText().Text(L"idle");
            break;
        case ReceiverState::failed:
            m_active_service_id = 0u;
            RfLiveBadgeText().Text(L"ERROR");
            RfStatusValueText().Text(L"ERROR");
            RfStatusDetailText().Text(status_text(status.message));
            SetReceiverControlsEnabled(true);
            StartButton().IsEnabled(true);
            StartButtonLabel().Text(L"Start");
            StatusSyncText().Text(L"error");
            break;
        }

        if (status.is_error)
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
        GainNumberBox().IsEnabled(enabled);
    }

    void MainWindow::ShowWorkspace(bool rf_measurements)
    {
        bool const changed = m_rf_workspace_active != rf_measurements;
        m_rf_workspace_active = rf_measurements;
        ReceiverWorkspaceMain().Visibility(rf_measurements ? Visibility::Collapsed : Visibility::Visible);
        ReceiverTimelinePanel().Visibility(rf_measurements ? Visibility::Collapsed : Visibility::Visible);
        RfWorkspace().Visibility(rf_measurements ? Visibility::Visible : Visibility::Collapsed);
        ReceiverNavIndicator().Visibility(rf_measurements ? Visibility::Collapsed : Visibility::Visible);
        RfNavIndicator().Visibility(rf_measurements ? Visibility::Visible : Visibility::Collapsed);
        ReceiverNavigationButton().Opacity(rf_measurements ? 0.70 : 1.0);
        RfNavigationButton().Opacity(rf_measurements ? 1.0 : 0.70);
        RfToolbarButton().Opacity(rf_measurements ? 1.0 : 0.82);
        ApplyWorkspaceWidth(RootLayout().ActualWidth());
        if (rf_measurements)
        {
            UpdateRfContext();
        }
        if (changed)
        {
            AddTimelineEvent(rf_measurements
                ? hstring{ L"Opened RF Measurements workspace" }
                : hstring{ L"Opened Receiver workspace" });
        }
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
        RfContextText().Text(device + L" · Serial not reported · Channel " + channel + L" · " + frequency);

        double gain = GainNumberBox().Value();
        if (!std::isfinite(gain))
        {
            gain = 40.0;
        }
        wchar_t gain_text[32]{};
        swprintf_s(gain_text, L"%.0f dB", gain);
        RfConfigGainText().Text(gain_text);
        RfConfigAgcText().Text(L"Disabled");
        RfConfigAntennaText().Text(L"A");
        RfConfigCalibrationText().Text(L"Not configured");
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
        UpdateRfContext();
        RfLiveBadgeText().Text(L"● LIVE");
        StatusCalibrationText().Text(L"Uncalibrated");

        RfPowerValueText().Text(snapshot.adc_level_valid
            ? format_float(snapshot.adc_level_dbfs, L" dBFS")
            : hstring{ L"— dBFS" });
        RfSummaryPowerText().Text(snapshot.adc_level_valid
            ? format_float(snapshot.adc_level_dbfs, L" dBFS")
            : hstring{ L"— dBFS" });
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
            ? hstring{ L"Residual " } + format_float(snapshot.fine_cfo_hz, L" Hz")
            : hstring{ L"Residual — Hz" });
        RfOverrunValueText().Text(to_hstring(snapshot.dropped_blocks));
        RfConfigSampleRateText().Text(snapshot.sample_rate_hz != 0u
            ? format_float(static_cast<float>(snapshot.sample_rate_hz) / 1000000.0f, L" MS/s", 3)
            : hstring{ L"— MS/s" });

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

    void MainWindow::ApplySnapshot(dab_receive_snapshot_t const& snapshot)
    {
        DataModeText().Text(L"LIVE METRICS");
        DataSeamText().Text(L"snapshot " + to_hstring(snapshot.sequence));
        StatusSyncText().Text(snapshot.locked != 0 ? L"synchronized" : L"acquiring");
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
    }

    void MainWindow::ApplyCatalog(dab_receive_catalog_t const& catalog)
    {
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

        m_latest_catalog = catalog;
        m_have_catalog = true;

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
    }

    void MainWindow::ClearCatalogView()
    {
        m_have_catalog = false;
        m_latest_catalog = {};
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
    }

    void MainWindow::UpdatePlots(dab::measurement::MeasurementFrame const& frame)
    {
        DataModeText().Text(L"HIGH-RATE MEASUREMENTS");
        DataSeamText().Text(L"measurement " + to_hstring(frame.sequence));
        SampleCounterText().Text(L"measurement " + to_hstring(frame.sequence));

        if (frame.spectrum_bin_count != 0u)
        {
            PointCollection spectrum_points;
            constexpr uint32_t display_points = 181u;
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
                float const x = 900.0f * static_cast<float>(point) /
                                static_cast<float>(display_points - 1u);
                float const y = std::clamp(240.0f - ((level + 120.0f) * 1.85f), 12.0f, 245.0f);
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

        auto constellation = ConstellationCanvas().Children();
        while (constellation.Size() > 2u)
        {
            constellation.RemoveAtEnd();
        }
        auto const dot_brush = SolidColorBrush(ColorHelper::FromArgb(185u, 84u, 226u, 255u));
        uint32_t const constellation_points = (std::min)(
            frame.constellation_point_count,
            256u);
        for (uint32_t point = 0u; point < constellation_points; ++point)
        {
            uint32_t const source = (point * frame.constellation_point_count) /
                                    constellation_points;
            double const x = std::clamp(260.0 + (180.0 * frame.constellation_i[source]), 10.0, 506.0);
            double const y = std::clamp(110.0 - (82.0 * frame.constellation_q[source]), 10.0, 206.0);
            Microsoft::UI::Xaml::Shapes::Ellipse dot;
            dot.Width(3.0);
            dot.Height(3.0);
            dot.Fill(dot_brush);
            Canvas::SetLeft(dot, x);
            Canvas::SetTop(dot, y);
            constellation.Append(dot);
        }

        if (frame.cir_bin_count != 0u)
        {
            PointCollection cir_points;
            constexpr uint32_t display_points = 161u;
            uint32_t peak_bin = 0u;
            float peak_level = frame.cir_power_db[0u];
            for (uint32_t bin = 1u; bin < frame.cir_bin_count; ++bin)
            {
                if (frame.cir_power_db[bin] > peak_level)
                {
                    peak_level = frame.cir_power_db[bin];
                    peak_bin = bin;
                }
            }
            for (uint32_t point = 0u; point < display_points; ++point)
            {
                uint32_t const begin = (point * frame.cir_bin_count) / display_points;
                uint32_t const end = (std::max)(begin + 1u,
                    ((point + 1u) * frame.cir_bin_count) / display_points);
                float level = -120.0f;
                for (uint32_t bin = begin; bin < (std::min)(end, frame.cir_bin_count); ++bin)
                {
                    level = (std::max)(level, frame.cir_power_db[bin]);
                }
                float const x = 800.0f * static_cast<float>(point) /
                                static_cast<float>(display_points - 1u);
                float const y = std::clamp(20.0f + (-level * 2.65f), 12.0f, 185.0f);
                cir_points.Append(Point{ x, y });
            }
            CirTrace().Points(cir_points);

            wchar_t cir_status[96]{};
            double const peak_delay_us = static_cast<double>(peak_bin) *
                                         static_cast<double>(frame.cir_sample_period_ns) / 1000.0;
            swprintf_s(cir_status,
                       L"main path %.2f µs · %u taps",
                       peak_delay_us,
                       frame.cir_bin_count);
            CirStatusText().Text(cir_status);
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
    }

    void MainWindow::AddTimelineEvent(hstring const& message)
    {
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
    }
}
