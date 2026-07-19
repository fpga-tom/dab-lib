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
