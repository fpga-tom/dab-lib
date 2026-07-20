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
using namespace Microsoft::UI::Xaml::Controls::Primitives;
using namespace Microsoft::UI::Xaml::Input;
using namespace Microsoft::UI::Xaml::Media;
using namespace Microsoft::UI::Xaml::Shapes;

namespace
{
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
        ResetRfSyncDiagnostics();
        ResetRfChannelDiagnostics();
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
            break;
        case ReceiverState::failed:
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
        if (rf_measurements && m_cir_panel_maximized)
        {
            SetCirPanelMaximized(false);
        }
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

        RfInputGainText().Text(snapshot.device_gain_valid != 0
            ? format_integer(snapshot.device_gain_db, L" dB")
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
        else
        {
            RfWarningsText().Text(L"Input power is ADC-referenced (dBFS) until an RF calibration profile is configured.");
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
        }
        if (snapshot.sync_reacquisition_count > m_rf_sync_last_reacquisition_count)
        {
            AddRfSyncEvent(hstring{ L"Frame lock reacquired after " } +
                           format_float(snapshot.sync_acquisition_time_ms, L" ms", 1));
        }
        if (locked && !m_rf_sync_seen_lock)
        {
            m_rf_sync_seen_lock = true;
            AddRfSyncEvent(hstring{ L"Initial frame lock acquired after " } +
                           format_float(snapshot.sync_acquisition_time_ms, L" ms", 1));
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

    void MainWindow::UpdatePlots(dab::measurement::MeasurementFrame const& frame)
    {
        DataModeText().Text(L"HIGH-RATE MEASUREMENTS");
        DataSeamText().Text(L"measurement " + to_hstring(frame.sequence));
        SampleCounterText().Text(L"measurement " + to_hstring(frame.sequence));

        if (frame.spectrum_bin_count != 0u)
        {
            UpdateRfSpectrum(frame);
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
