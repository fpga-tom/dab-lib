#include "pch.h"
#include "MainWindow.xaml.h"

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

        if (!m_receiver.Start(*settings))
        {
            AddTimelineEvent(L"Receiver worker is already active");
            return;
        }

        SetReceiverControlsEnabled(false);
        StartButton().IsEnabled(true);
        StartButtonLabel().Text(L"Stop");
        StatusSyncText().Text(L"opening");
        DataModeText().Text(L"LIVE METRICS");
        DataSeamText().Text(L"awaiting receiver snapshots");
        AddTimelineEvent(L"Opening real receiver worker");
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
        m_receiver.RequestStop();
    }

    void MainWindow::OnTimerTick(IInspectable const&, IInspectable const&)
    {
        ++m_tick;

        dab::measurement::ReceiverStatus receiver_status;
        uint64_t status_version = 0;
        if (m_receiver.TryReadStatus(m_receiver_status_version, receiver_status, status_version))
        {
            m_receiver_status_version = status_version;
            HandleReceiverStatus(receiver_status);
        }
        m_receiver.JoinFinished();

        dab_receive_snapshot_t snapshot{};
        uint64_t snapshot_version = 0;
        if (m_mailbox.TryReadLatest(m_mailbox_version, snapshot, snapshot_version))
        {
            m_mailbox_version = snapshot_version;
            ApplySnapshot(snapshot);
            UpdatePlots(snapshot);
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
        settings.service_id = 0x00001001u;
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

    void MainWindow::HandleReceiverStatus(dab::measurement::ReceiverStatus const& status)
    {
        AddTimelineEvent(status_text(status.message));
        DataSeamText().Text(status_text(status.message));

        using dab::measurement::ReceiverState;
        switch (status.state)
        {
        case ReceiverState::starting:
            SetReceiverControlsEnabled(false);
            StartButton().IsEnabled(true);
            StartButtonLabel().Text(L"Stop");
            StatusSyncText().Text(L"opening");
            break;
        case ReceiverState::running:
            SetReceiverControlsEnabled(false);
            StartButton().IsEnabled(true);
            StartButtonLabel().Text(L"Stop");
            if (StatusSyncText().Text() == L"opening")
            {
                StatusSyncText().Text(L"acquiring");
            }
            break;
        case ReceiverState::stopping:
            SetReceiverControlsEnabled(false);
            StartButton().IsEnabled(false);
            StartButtonLabel().Text(L"Stopping...");
            StatusSyncText().Text(L"stopping");
            break;
        case ReceiverState::stopped:
            SetReceiverControlsEnabled(true);
            StartButton().IsEnabled(true);
            StartButtonLabel().Text(L"Start");
            StatusSyncText().Text(L"idle");
            break;
        case ReceiverState::failed:
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

    void MainWindow::ApplySnapshot(dab_receive_snapshot_t const& snapshot)
    {
        DataModeText().Text(L"LIVE METRICS");
        DataSeamText().Text(L"snapshot " + to_hstring(snapshot.sequence));
        StatusSyncText().Text(snapshot.locked != 0 ? L"synchronized" : L"acquiring");
        SampleRateText().Text(snapshot.sample_rate_hz != 0u
            ? format_float(static_cast<float>(snapshot.sample_rate_hz) / 1000000.0f, L" MS/s", 3)
            : hstring{ L"— MS/s" });

        MerValueText().Text(snapshot.cn_valid
            ? format_float(snapshot.cn_db - 1.1f, L" dB")
            : hstring{ L"— dB" });
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

        uint32_t const fib_total = snapshot.fic_blocks_decoded_last_frame * 3u;
        CrcValueText().Text(fib_total != 0u
            ? to_hstring(snapshot.fic_valid_fibs_last_frame) + L" / " + to_hstring(fib_total)
            : hstring{ L"— / —" });
        double const crc_ratio = fib_total != 0u
            ? static_cast<double>(snapshot.fic_valid_fibs_last_frame) / static_cast<double>(fib_total)
            : 0.0;
        FicProgress().Value(crc_ratio);
        BerValueText().Text(fib_total == 0u ? L"—" : (crc_ratio >= 1.0 ? L"FIC clean" : L"CRC loss"));
        SampleCounterText().Text(L"frame " + to_hstring(snapshot.sequence));
    }

    void MainWindow::UpdatePlots(dab_receive_snapshot_t const& snapshot)
    {
        PointCollection spectrum_points;
        constexpr uint32_t point_count = 91u;
        double const phase = static_cast<double>(snapshot.sequence) * 0.12;

        for (uint32_t i = 0u; i < point_count; ++i)
        {
            double const x = 900.0 * static_cast<double>(i) / static_cast<double>(point_count - 1u);
            double const offset = (x - 450.0) / 310.0;
            double const envelope = std::exp(-(offset * offset) * 1.7);
            double const ripple = (7.0 * std::sin((x * 0.045) + phase)) +
                                  (3.0 * std::sin((x * 0.13) - (phase * 0.7)));
            double const y = std::clamp(225.0 - (118.0 * envelope) - ripple, 18.0, 242.0);
            spectrum_points.Append(Point{ static_cast<float>(x), static_cast<float>(y) });
        }
        SpectrumTrace().Points(spectrum_points);

        Canvas::SetTop(WaterfallCursor(), static_cast<double>((snapshot.sequence * 7u) % 120u) + 2.0);

        auto children = ConstellationCanvas().Children();
        while (children.Size() > 2u)
        {
            children.RemoveAtEnd();
        }

        auto const dot_brush = SolidColorBrush(ColorHelper::FromArgb(210u, 84u, 226u, 255u));
        for (uint32_t i = 0u; i < 24u; ++i)
        {
            double const local_phase = phase + (static_cast<double>(i) * 1.91);
            double const center_x = (i % 2u == 0u) ? 130.0 : 392.0;
            double const center_y = ((i / 2u) % 2u == 0u) ? 53.0 : 169.0;
            double const radius = 4.0 + static_cast<double>(i % 5u);
            Microsoft::UI::Xaml::Shapes::Ellipse dot;
            dot.Width(4.0);
            dot.Height(4.0);
            dot.Fill(dot_brush);
            Canvas::SetLeft(dot, center_x + (radius * std::cos(local_phase)));
            Canvas::SetTop(dot, center_y + (radius * std::sin(local_phase * 1.17)));
            children.Append(dot);
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
