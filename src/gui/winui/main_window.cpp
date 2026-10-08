#include "main_window.hpp"

#include "dab/core/audio_eq.h"

#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Storage.Streams.h>
#include <winrt/Windows.UI.h>
#include <winrt/Windows.UI.Text.h>
#include <winrt/Microsoft.UI.Xaml.Controls.h>
#include <winrt/Microsoft.UI.Xaml.Controls.Primitives.h>
#include <winrt/Microsoft.UI.Xaml.Media.h>
#include <winrt/Microsoft.UI.Xaml.Media.Imaging.h>
#include <winrt/Microsoft.UI.Xaml.Shapes.h>

#include <windows.h>
#include <shlobj.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <utility>

using namespace winrt;
using namespace Windows::Foundation;
using namespace Windows::Storage::Streams;
using namespace Microsoft::UI::Xaml;
using namespace Microsoft::UI::Xaml::Controls;
using namespace Microsoft::UI::Xaml::Media;
using namespace Microsoft::UI::Xaml::Media::Imaging;
using namespace Microsoft::UI::Xaml::Shapes;
using XamlRectangle = Microsoft::UI::Xaml::Shapes::Rectangle;

namespace {

struct EqualizerPreset {
    wchar_t const *name;
    wchar_t const *description;
    std::array<int, DAB_AUDIO_EQ_BAND_COUNT> gains;
};

constexpr std::array<EqualizerPreset, 7> kEqualizerPresets = {{
    {L"Flat", L"Balanced response for general listening", {0, 0, 0, 0, 0, 0, 0, 0, 0, 0}},
    {L"Rock", L"Extra punch in bass and treble", {5, 4, 2, 0, -1, 1, 3, 4, 5, 4}},
    {L"Pop", L"Forward vocals with bright upper mids", {2, 3, 4, 3, 1, 0, 2, 3, 4, 3}},
    {L"Jazz", L"Warm low end and open detail", {3, 2, 1, 0, 1, 2, 3, 2, 1, 1}},
    {L"Classical", L"Light bass lift and airy highs", {1, 1, 0, -1, 0, 2, 3, 4, 4, 3}},
    {L"Electronic", L"Deep bass with crisp top end", {6, 5, 3, 1, 0, 2, 4, 5, 5, 4}},
    {L"Custom", L"Manual band adjustment", {0, 0, 0, 0, 0, 0, 0, 0, 0, 0}},
}};

constexpr std::array<wchar_t const *, DAB_AUDIO_EQ_BAND_COUNT> kEqualizerBands = {
    L"60 Hz", L"170 Hz", L"310 Hz", L"600 Hz", L"1 kHz",
    L"3 kHz", L"6 kHz", L"10 kHz", L"12 kHz", L"16 kHz"
};

constexpr std::array<float, 10> kFrequencyMarkers = {
    20.0f, 50.0f, 100.0f, 200.0f, 500.0f,
    1000.0f, 2000.0f, 5000.0f, 10000.0f, 20000.0f
};

constexpr int kDbLabels[] = {0, -12, -24, -36, -48, -60, -70};
constexpr wchar_t kSettingsRegistryPath[] = L"Software\\dab-lib\\WinUI";

Windows::UI::Color Rgb(uint8_t red, uint8_t green, uint8_t blue, uint8_t alpha = 255) {
    return Windows::UI::Color{alpha, red, green, blue};
}

SolidColorBrush MakeBrush(uint8_t red, uint8_t green, uint8_t blue, uint8_t alpha = 255) {
    return SolidColorBrush(Rgb(red, green, blue, alpha));
}

TextBlock MakeText(std::wstring_view text, double size = 14.0, bool bold = false) {
    TextBlock block;
    block.Text(std::wstring(text));
    block.FontSize(size);
    if (bold) {
        block.FontWeight(Windows::UI::Text::FontWeights::SemiBold());
    }
    return block;
}

Border MakeCard(UIElement const &child, double padding = 16.0) {
    Border card;
    card.Background(MakeBrush(29, 38, 49));
    card.BorderBrush(MakeBrush(55, 69, 84));
    card.BorderThickness(Thickness{1.0, 1.0, 1.0, 1.0});
    card.CornerRadius(CornerRadius{8.0, 8.0, 8.0, 8.0});
    card.Padding(Thickness{padding, padding, padding, padding});
    card.Child(child);
    return card;
}

hstring FormatDb(int value) {
    wchar_t buffer[32]{};
    swprintf_s(buffer, L"%+d dB", value);
    return buffer;
}

hstring ToHString(std::string_view text) {
    if (text.empty()) {
        return {};
    }
    if (text.size() > static_cast<size_t>((std::numeric_limits<int>::max)())) {
        throw std::length_error("String is too long to convert to UTF-16.");
    }
    const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                                           static_cast<int>(text.size()), nullptr, 0);
    if (length == 0) {
        winrt::throw_last_error();
    }
    std::wstring wide(static_cast<size_t>(length), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                            static_cast<int>(text.size()), wide.data(), length) != length) {
        winrt::throw_last_error();
    }
    return hstring(wide);
}

bool ReadRegistryDword(wchar_t const *name, DWORD &value) {
    DWORD size = sizeof(value);
    return RegGetValueW(HKEY_CURRENT_USER, kSettingsRegistryPath, name,
                        RRF_RT_REG_DWORD, nullptr, &value, &size) == ERROR_SUCCESS;
}

bool WriteRegistryDword(wchar_t const *name, DWORD value) {
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kSettingsRegistryPath, 0, nullptr, 0,
                        KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS) {
        return false;
    }
    const LONG result = RegSetValueExW(key, name, 0, REG_DWORD,
                                       reinterpret_cast<BYTE const *>(&value), sizeof(value));
    RegCloseKey(key);
    return result == ERROR_SUCCESS;
}

std::string Trim(std::string value) {
    const size_t first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return {};
    }
    const size_t last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

std::vector<std::string> SplitTabs(std::string const &line) {
    std::vector<std::string> fields;
    size_t start = 0;
    for (;;) {
        const size_t end = line.find('\t', start);
        fields.emplace_back(line.substr(start, end == std::string::npos ? end : end - start));
        if (end == std::string::npos) {
            return fields;
        }
        start = end + 1;
    }
}

bool ParseServiceId(std::string const &text, uint32_t &serviceId) {
    if (text.empty()) {
        return false;
    }
    char *end = nullptr;
    const unsigned long value = std::strtoul(text.c_str(), &end, 0);
    if (end == text.c_str() || *end != '\0' || value > UINT32_MAX) {
        return false;
    }
    serviceId = static_cast<uint32_t>(value);
    return true;
}

std::wstring LocalAppDataPath() {
    PWSTR path = nullptr;
    const HRESULT result = SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_CREATE, nullptr, &path);
    if (FAILED(result) || path == nullptr) {
        return {};
    }
    std::wstring value(path);
    CoTaskMemFree(path);
    return value;
}

bool EnvEnabled(wchar_t const *name, bool fallback = false) {
    wchar_t value[16]{};
    const DWORD length = GetEnvironmentVariableW(name, value, static_cast<DWORD>(std::size(value)));
    if (length == 0 || length >= std::size(value)) {
        return fallback;
    }
    return _wcsicmp(value, L"0") != 0 &&
           _wcsicmp(value, L"false") != 0 &&
           _wcsicmp(value, L"off") != 0;
}

std::wstring CurrentSystemTime() {
    const std::time_t now = std::time(nullptr);
    std::tm localTime{};
    localtime_s(&localTime, &now);
    std::wostringstream text;
    text << L"System: " << std::put_time(&localTime, L"%H:%M:%S");
    return text.str();
}

std::string FormatReceiverStatus(dab_gui_playback_status_t const &status) {
    std::ostringstream text;
    if (!status.locked) {
        text << "Receiver: acquiring, PRS " << std::fixed << std::setprecision(3) << status.prs_score;
        return text.str();
    }
    text << "Receiver: locked";
    if (status.has_rf_level) {
        text << ", RF " << std::fixed << std::setprecision(1) << status.rf_level_dbfs << " dBFS";
    }
    if (status.has_receiver_snr) {
        text << ", SNR " << std::fixed << std::setprecision(1) << status.receiver_snr_db << " dB";
    }
    if (status.has_spectrum_cn) {
        text << ", C/N " << std::fixed << std::setprecision(1) << status.spectrum_cn_db << " dB";
    }
    text << ", PRS " << std::fixed << std::setprecision(3) << status.prs_score;
    return text.str();
}

float BinToFrequency(size_t index, size_t count) {
    constexpr float minHz = 20.0f;
    constexpr float maxHz = 20000.0f;
    const float fraction = count > 1 ? static_cast<float>(index) / static_cast<float>(count - 1) : 0.0f;
    return minHz * std::pow(maxHz / minHz, fraction);
}

float VisualLevel(float level) {
    return std::pow(std::clamp(level, 0.0f, 1.0f), 0.68f);
}

}  // namespace

MainWindow::MainWindow(bool showEnsembleTime)
    : window_(Window()),
      showEnsembleTime_(showEnsembleTime),
      showReceiverStatus_(EnvEnabled(L"DAB_GUI_SHOW_RECEIVER")) {
    dab_gui_sdr_default_settings(&sdrSettings_);
    LoadSdrSettings();
    LoadSpectrumSettings();
    ApplySdrSettings();
    OutputDebugStringW(L"MainWindow: building UI\n");
    BuildUi(showEnsembleTime);
    OutputDebugStringW(L"MainWindow: UI built\n");

    statusTimer_ = DispatcherTimer();
    statusTimer_.Interval(std::chrono::milliseconds(400));
    statusTimer_.Tick([this](IInspectable const &, IInspectable const &) {
        RefreshPlaybackStatus();
        DrainScanEvents();
    });
    statusTimer_.Start();

    spectrumTimer_ = DispatcherTimer();
    spectrumTimer_.Interval(std::chrono::milliseconds(40));
    spectrumTimer_.Tick([this](IInspectable const &, IInspectable const &) {
        dab_gui_playback_spectrum_t spectrum{};
        if (dab_gui_playback_get_spectrum(&spectrum) == 0 && spectrum.valid && spectrum.bin_count > 0) {
            const size_t count = std::min(spectrumLevels_.size(), static_cast<size_t>(spectrum.bin_count));
            for (size_t index = 0; index < count; ++index) {
                spectrumLevels_[index] = std::clamp(spectrum.bins[index], 0.0f, 1.0f);
                spectrumPeaks_[index] = std::max(spectrumLevels_[index], spectrumPeaks_[index] * 0.91f);
            }
            for (size_t index = count; index < spectrumLevels_.size(); ++index) {
                spectrumLevels_[index] *= 0.90f;
                spectrumPeaks_[index] *= 0.86f;
            }
        } else {
            for (float &level : spectrumLevels_) {
                level *= 0.88f;
            }
            for (float &peak : spectrumPeaks_) {
                peak *= 0.88f;
            }
        }
        DrawSpectrum();
    });
    spectrumTimer_.Start();

    window_.Closed([this](IInspectable const &, WindowEventArgs const &) {
        statusTimer_.Stop();
        spectrumTimer_.Stop();
        StopScanWorker();
        if (dab_gui_playback_is_running() != 0) {
            dab_gui_playback_stop();
        }
    });

    ReloadStationsFromScanResults();
    UpdatePlaybackControls();
    RefreshPlaybackStatus();
}

MainWindow::~MainWindow() {
    statusTimer_.Stop();
    spectrumTimer_.Stop();
    StopScanWorker();
    if (dab_gui_playback_is_running() != 0) {
        dab_gui_playback_stop();
    }
}

void MainWindow::Activate() {
    window_.Activate();
}

void MainWindow::BuildUi(bool showEnsembleTime) {
    OutputDebugStringW(L"BuildUi: begin\n");
    window_.Title(L"dab-lib DAB SDR");

    StackPanel page;
    page.Spacing(14.0);
    page.Padding(Thickness{18.0, 16.0, 18.0, 18.0});
    page.Background(MakeBrush(16, 24, 32));

    StackPanel heading;
    heading.Orientation(Orientation::Horizontal);
    heading.VerticalAlignment(VerticalAlignment::Center);
    heading.Spacing(12.0);
    TextBlock title = MakeText(L"dab-lib", 26.0, true);
    heading.Children().Append(title);
    TextBlock subtitle = MakeText(L"DAB+ SDR receiver", 14.0);
    subtitle.Foreground(MakeBrush(155, 172, 188));
    subtitle.VerticalAlignment(VerticalAlignment::Center);
    heading.Children().Append(subtitle);

    StackPanel settingsButtons;
    settingsButtons.Orientation(Orientation::Horizontal);
    settingsButtons.Spacing(8.0);
    Button sdrSettingsButton;
    sdrSettingsButton.Content(box_value(L"SDR settings"));
    sdrSettingsButton.Click([this](IInspectable const &, RoutedEventArgs const &) {
        OnSdrSettings();
    });
    settingsButtons.Children().Append(sdrSettingsButton);
    Button spectrumSettingsButton;
    spectrumSettingsButton.Content(box_value(L"Spectrum settings"));
    spectrumSettingsButton.Click([this](IInspectable const &, RoutedEventArgs const &) {
        OnSpectrumSettings();
    });
    settingsButtons.Children().Append(spectrumSettingsButton);

    Grid topRow;
    topRow.ColumnDefinitions().Append(ColumnDefinition());
    ColumnDefinition settingsColumn;
    settingsColumn.Width(GridLength{1.0, GridUnitType::Auto});
    topRow.ColumnDefinitions().Append(settingsColumn);
    Grid::SetColumn(settingsButtons, 1);
    topRow.Children().Append(heading);
    topRow.Children().Append(settingsButtons);
    page.Children().Append(topRow);

    StackPanel content;
    content.Spacing(14.0);
    OutputDebugStringW(L"BuildUi: spectrum\n");
    BuildSpectrumPanel(content);
    OutputDebugStringW(L"BuildUi: equalizer\n");
    BuildEqualizerPanel(content);
    OutputDebugStringW(L"BuildUi: playback\n");
    BuildPlaybackPanel(content);
    OutputDebugStringW(L"BuildUi: scan\n");
    BuildScanPanel(content);
    OutputDebugStringW(L"BuildUi: compose\n");

    ScrollViewer scrollViewer;
    scrollViewer.VerticalScrollBarVisibility(ScrollBarVisibility::Auto);
    scrollViewer.HorizontalScrollBarVisibility(ScrollBarVisibility::Disabled);
    scrollViewer.Content(content);
    page.Children().Append(scrollViewer);
    window_.Content(page);
    OutputDebugStringW(L"BuildUi: content set\n");

    currentTimeLabel_.Text(CurrentSystemTime());
    ensembleTimeLabel_.Visibility(showEnsembleTime ? Visibility::Visible : Visibility::Collapsed);
}

void MainWindow::BuildSpectrumPanel(StackPanel const &parent) {
    StackPanel panel;
    panel.Spacing(8.0);
    StackPanel heading;
    heading.Orientation(Orientation::Horizontal);
    heading.Spacing(10.0);
    heading.Children().Append(MakeText(L"Spectrum analyzer", 17.0, true));
    TextBlock mode = MakeText(L"LOG FREQUENCY  ·  dBFS", 12.0);
    mode.Foreground(MakeBrush(138, 158, 176));
    mode.VerticalAlignment(VerticalAlignment::Center);
    heading.Children().Append(mode);
    panel.Children().Append(heading);

    spectrumCanvas_ = Canvas();
    spectrumCanvas_.Height(230.0);
    spectrumCanvas_.MinHeight(190.0);
    spectrumCanvas_.HorizontalAlignment(HorizontalAlignment::Stretch);
    spectrumCanvas_.Background(MakeBrush(10, 16, 22));
    panel.Children().Append(spectrumCanvas_);
    parent.Children().Append(MakeCard(panel, 14.0));
}

void MainWindow::BuildEqualizerPanel(StackPanel const &parent) {
    StackPanel panel;
    panel.Spacing(8.0);

    Grid heading;
    heading.ColumnDefinitions().Append(ColumnDefinition());
    ColumnDefinition presetColumn;
    presetColumn.Width(GridLength{1.0, GridUnitType::Auto});
    heading.ColumnDefinitions().Append(presetColumn);
    heading.Children().Append(MakeText(L"Equalizer bands", 17.0, true));

    StackPanel presetRow;
    presetRow.Orientation(Orientation::Horizontal);
    presetRow.Spacing(8.0);
    presetRow.VerticalAlignment(VerticalAlignment::Center);
    TextBlock presetTitle = MakeText(L"Preset", 13.0);
    presetTitle.VerticalAlignment(VerticalAlignment::Center);
    presetRow.Children().Append(presetTitle);
    equalizerPresetChoice_ = ComboBox();
    equalizerPresetChoice_.MinWidth(145.0);
    for (auto const &preset : kEqualizerPresets) {
        ComboBoxItem item;
        item.Content(box_value(preset.name));
        equalizerPresetChoice_.Items().Append(item);
    }
    equalizerPresetChoice_.SelectedIndex(0);
    presetRow.Children().Append(equalizerPresetChoice_);
    Grid::SetColumn(presetRow, 1);
    heading.Children().Append(presetRow);
    panel.Children().Append(heading);

    equalizerPresetDescription_ = MakeText(kEqualizerPresets[0].description, 12.0);
    equalizerPresetDescription_.Foreground(MakeBrush(155, 172, 188));
    panel.Children().Append(equalizerPresetDescription_);

    StackPanel sliders;
    sliders.Orientation(Orientation::Horizontal);
    sliders.HorizontalAlignment(HorizontalAlignment::Stretch);
    sliders.Spacing(4.0);

    for (size_t index = 0; index < kEqualizerBands.size(); ++index) {
        StackPanel column;
        column.HorizontalAlignment(HorizontalAlignment::Center);
        column.VerticalAlignment(VerticalAlignment::Stretch);
        column.Spacing(4.0);
        column.Width(82.0);

        TextBlock valueLabel = MakeText(L"0 dB", 12.0);
        valueLabel.HorizontalAlignment(HorizontalAlignment::Center);
        equalizerValueLabels_.push_back(valueLabel);
        column.Children().Append(valueLabel);

        Slider slider;
        slider.Orientation(Orientation::Vertical);
        slider.IsDirectionReversed(true);
        slider.Minimum(-12.0);
        slider.Maximum(12.0);
        slider.Value(0.0);
        slider.StepFrequency(1.0);
        slider.Height(112.0);
        slider.HorizontalAlignment(HorizontalAlignment::Center);
        equalizerSliders_.push_back(slider);
        slider.ValueChanged([this, valueLabel](IInspectable const &,
            Microsoft::UI::Xaml::Controls::Primitives::RangeBaseValueChangedEventArgs const &args) {
            const int gain = static_cast<int>(std::lround(args.NewValue()));
            valueLabel.Text(FormatDb(gain));
            if (!applyingEqualizerPreset_) {
                equalizerPresetChoice_.SelectedIndex(static_cast<int>(kEqualizerPresets.size() - 1));
                equalizerPresetDescription_.Text(kEqualizerPresets.back().description);
                PushEqualizerToBackend();
            }
        });
        column.Children().Append(slider);

        TextBlock band = MakeText(kEqualizerBands[index], 11.0);
        band.HorizontalAlignment(HorizontalAlignment::Center);
        band.Foreground(MakeBrush(166, 180, 194));
        column.Children().Append(band);
        sliders.Children().Append(column);
    }
    panel.Children().Append(sliders);

    equalizerPresetChoice_.SelectionChanged([this](IInspectable const &, SelectionChangedEventArgs const &) {
        const int index = equalizerPresetChoice_.SelectedIndex();
        if (index >= 0 && static_cast<size_t>(index) < kEqualizerPresets.size()) {
            ApplyEqualizerPreset(static_cast<size_t>(index));
        }
    });

    parent.Children().Append(MakeCard(panel, 14.0));
    ApplyEqualizerPreset(0);
}

void MainWindow::BuildPlaybackPanel(StackPanel const &parent) {
    Grid content;
    content.ColumnDefinitions().Append(ColumnDefinition());
    ColumnDefinition mainColumn;
    mainColumn.Width(GridLength{3.0, GridUnitType::Star});
    content.ColumnDefinitions().Append(mainColumn);
    if (showReceiverStatus_) {
        ColumnDefinition receiverColumn;
        receiverColumn.Width(GridLength{1.5, GridUnitType::Star});
        content.ColumnDefinitions().Append(receiverColumn);
    }

    StackPanel logoContainer;
    logoContainer.Width(200.0);
    logoContainer.Height(120.0);
    logoContainer.HorizontalAlignment(HorizontalAlignment::Left);
    logoContainer.VerticalAlignment(VerticalAlignment::Top);
    logoContainer.Background(MakeBrush(24, 30, 38));
    stationLogo_ = Image();
    stationLogo_.Stretch(Stretch::Uniform);
    stationLogo_.Visibility(Visibility::Collapsed);
    logoContainer.Children().Append(stationLogo_);
    content.Children().Append(logoContainer);

    StackPanel controls;
    controls.Spacing(6.0);
    Grid controlsHeader;
    controlsHeader.ColumnDefinitions().Append(ColumnDefinition());
    ColumnDefinition clockColumn;
    clockColumn.Width(GridLength{1.0, GridUnitType::Auto});
    controlsHeader.ColumnDefinitions().Append(clockColumn);

    StackPanel stationIdentity;
    stationIdentity.Orientation(Orientation::Horizontal);
    stationIdentity.Spacing(12.0);
    stationIdentity.Children().Append(MakeText(L"Playback controls", 17.0, true));
    ensembleLabel_ = MakeText(L"Ensemble: -", 13.0);
    ensembleLabel_.Foreground(MakeBrush(155, 172, 188));
    ensembleLabel_.VerticalAlignment(VerticalAlignment::Center);
    stationIdentity.Children().Append(ensembleLabel_);
    controlsHeader.Children().Append(stationIdentity);

    StackPanel clocks;
    clocks.HorizontalAlignment(HorizontalAlignment::Right);
    ensembleTimeLabel_ = MakeText(L"Ensemble: --:--", 12.0);
    ensembleTimeLabel_.HorizontalAlignment(HorizontalAlignment::Right);
    currentTimeLabel_ = MakeText(L"System: --:--:--", 12.0);
    currentTimeLabel_.HorizontalAlignment(HorizontalAlignment::Right);
    clocks.Children().Append(ensembleTimeLabel_);
    clocks.Children().Append(currentTimeLabel_);
    Grid::SetColumn(clocks, 1);
    controlsHeader.Children().Append(clocks);
    controls.Children().Append(controlsHeader);

    nowPlayingLabel_ = MakeText(L"Now playing: -", 15.0, true);
    nowPlayingLabel_.Foreground(MakeBrush(226, 234, 242));
    controls.Children().Append(nowPlayingLabel_);

    controlStatusLabel_ = MakeText(L"Scan stations to populate the list", 12.0);
    controlStatusLabel_.Foreground(MakeBrush(155, 172, 188));
    controls.Children().Append(controlStatusLabel_);

    TextBlock stationLabel = MakeText(L"Station", 12.0, true);
    stationLabel.Margin(Thickness{0.0, 4.0, 0.0, 0.0});
    controls.Children().Append(stationLabel);
    stationChoice_ = ComboBox();
    stationChoice_.PlaceholderText(L"Select a DAB station");
    stationChoice_.HorizontalAlignment(HorizontalAlignment::Stretch);
    stationChoice_.SelectionChanged([this](IInspectable const &, SelectionChangedEventArgs const &) {
        if (dab_gui_playback_is_running() != 0) {
            controlStatusLabel_.Text(L"Switching to selected station...");
            StopPlayback();
            StartPlaybackForCurrentSelection();
        } else if (stationChoice_.SelectedIndex() >= 0) {
            controlStatusLabel_.Text(L"Station selected. Press Start to play.");
        }
        UpdatePlaybackControls();
    });
    controls.Children().Append(stationChoice_);

    StackPanel buttonRow;
    buttonRow.Orientation(Orientation::Horizontal);
    buttonRow.Spacing(8.0);
    startButton_ = Button();
    startButton_.Content(box_value(L"Start"));
    startButton_.MinWidth(100.0);
    startButton_.Click([this](IInspectable const &, RoutedEventArgs const &) {
        StartPlaybackForCurrentSelection();
    });
    stopButton_ = Button();
    stopButton_.Content(box_value(L"Stop"));
    stopButton_.MinWidth(100.0);
    stopButton_.IsEnabled(false);
    stopButton_.Click([this](IInspectable const &, RoutedEventArgs const &) {
        StopPlayback();
    });
    scanButton_ = Button();
    scanButton_.Content(box_value(L"Scan"));
    scanButton_.MinWidth(100.0);
    scanButton_.Click([this](IInspectable const &, RoutedEventArgs const &) {
        if (scanWorker_.joinable() && !scanFinished_.load()) {
            CancelScan();
        } else {
            StartScan();
        }
    });
    buttonRow.Children().Append(startButton_);
    buttonRow.Children().Append(stopButton_);
    buttonRow.Children().Append(scanButton_);
    controls.Children().Append(buttonRow);
    Grid::SetColumn(controls, 1);
    content.Children().Append(controls);

    if (showReceiverStatus_) {
        StackPanel receiver;
        receiver.Spacing(6.0);
        receiver.Children().Append(MakeText(L"Receiver", 17.0, true));
        receiverStatusLabel_ = MakeText(L"Receiver: idle", 12.0);
        receiverStatusLabel_.TextWrapping(TextWrapping::Wrap);
        receiver.Children().Append(receiverStatusLabel_);
        Grid::SetColumn(receiver, 2);
        content.Children().Append(receiver);
    }

    parent.Children().Append(MakeCard(content, 14.0));
}

void MainWindow::BuildScanPanel(StackPanel const &parent) {
    StackPanel content;
    content.Spacing(8.0);
    content.Children().Append(MakeText(L"Channel scan", 16.0, true));
    scanStatusLabel_ = MakeText(L"Preparing tuner...", 13.0);
    content.Children().Append(scanStatusLabel_);
    scanChannelLabel_ = MakeText(L"Channel: --", 12.0);
    scanChannelLabel_.Foreground(MakeBrush(155, 172, 188));
    content.Children().Append(scanChannelLabel_);
    scanProgress_ = ProgressBar();
    scanProgress_.Minimum(0.0);
    scanProgress_.Maximum(1.0);
    scanProgress_.Value(0.0);
    content.Children().Append(scanProgress_);

    scanCancelButton_ = Button();
    scanCancelButton_.Content(box_value(L"Cancel scan"));
    scanCancelButton_.HorizontalAlignment(HorizontalAlignment::Right);
    scanCancelButton_.Click([this](IInspectable const &, RoutedEventArgs const &) {
        CancelScan();
    });
    content.Children().Append(scanCancelButton_);
    scanPanel_ = MakeCard(content, 14.0);
    scanPanel_.Visibility(Visibility::Collapsed);
    parent.Children().Append(scanPanel_);

    const std::wstring localData = LocalAppDataPath();
    if (localData.empty()) {
        controlStatusLabel_.Text(L"Could not locate the local application data folder.");
        return;
    }
    const std::filesystem::path appData = std::filesystem::path(localData) / L"dab-lib";
    std::error_code error;
    std::filesystem::create_directories(appData, error);
    if (error) {
        controlStatusLabel_.Text(L"Could not create the station scan results folder.");
        return;
    }
    scanResultsPath_ = (appData / L"dab_scan_results.tsv").native();
}

void MainWindow::DrawSpectrum() {
    if (!spectrumCanvas_) {
        return;
    }
    const double width = std::max(320.0, spectrumCanvas_.ActualWidth());
    const double height = std::max(190.0, spectrumCanvas_.ActualHeight());
    constexpr double left = 48.0;
    constexpr double right = 12.0;
    constexpr double top = 12.0;
    constexpr double bottom = 25.0;
    const double plotWidth = std::max(1.0, width - left - right);
    const double plotHeight = std::max(1.0, height - top - bottom);

    spectrumCanvas_.Children().Clear();
    XamlRectangle background;
    background.Width(width);
    background.Height(height);
    background.Fill(MakeBrush(10, 16, 22));
    spectrumCanvas_.Children().Append(background);

    auto addRect = [this](double x, double y, double rectWidth, double rectHeight, Windows::UI::Color color) {
        XamlRectangle rectangle;
        rectangle.Width(std::max(1.0, rectWidth));
        rectangle.Height(std::max(1.0, rectHeight));
        rectangle.Fill(SolidColorBrush(color));
        Canvas::SetLeft(rectangle, x);
        Canvas::SetTop(rectangle, y);
        spectrumCanvas_.Children().Append(rectangle);
    };
    auto addLabel = [this](std::wstring text, double x, double y, double fontSize = 10.0) {
        TextBlock label = MakeText(text, fontSize);
        label.Foreground(MakeBrush(133, 151, 168));
        Canvas::SetLeft(label, x);
        Canvas::SetTop(label, y);
        spectrumCanvas_.Children().Append(label);
    };

    for (int db : kDbLabels) {
        const double fraction = static_cast<double>(-db) / 70.0;
        const double y = top + fraction * plotHeight;
        addRect(left, y, plotWidth, 1.0, Rgb(db == 0 ? 65 : 34, db == 0 ? 81 : 45, db == 0 ? 96 : 57));
        addLabel(std::to_wstring(db), 6.0, y - 8.0);
    }

    auto frequencyToX = [plotWidth](float frequency) {
        const double minimum = std::log10(20.0);
        const double maximum = std::log10(20000.0);
        const double value = std::log10(std::clamp(static_cast<double>(frequency), 20.0, 20000.0));
        return left + ((value - minimum) / (maximum - minimum)) * plotWidth;
    };
    for (float frequency : kFrequencyMarkers) {
        const double x = frequencyToX(frequency);
        addRect(x, top, 1.0, plotHeight, Rgb(27, 39, 50));
        std::wostringstream label;
        if (frequency >= 1000.0f) {
            const float khz = frequency / 1000.0f;
            label << std::fixed << std::setprecision(khz >= 10.0f ? 0 : 1) << khz << L"k";
        } else {
            label << std::fixed << std::setprecision(0) << frequency;
        }
        addLabel(label.str(), x - 10.0, top + plotHeight + 4.0);
    }

    const size_t count = spectrumLevels_.size();
    if (spectrumDisplayMode_ == SpectrumDisplayMode::FftCurve) {
        PointCollection points;
        for (size_t index = 0; index < count; ++index) {
            const float frequency = BinToFrequency(index, count);
            const float level = VisualLevel(spectrumLevels_[index]);
            const double x = frequencyToX(frequency);
            const double y = top + (1.0 - level) * plotHeight;
            points.Append(Point{static_cast<float>(x), static_cast<float>(y)});
        }
        Microsoft::UI::Xaml::Shapes::Polyline curve;
        curve.Points(points);
        curve.Stroke(MakeBrush(116, 232, 242));
        curve.StrokeThickness(2.0);
        spectrumCanvas_.Children().Append(curve);

        PointCollection peaks;
        for (size_t index = 0; index < count; ++index) {
            const float frequency = BinToFrequency(index, count);
            const float level = VisualLevel(spectrumPeaks_[index]);
            peaks.Append(Point{static_cast<float>(frequencyToX(frequency)),
                               static_cast<float>(top + (1.0f - level) * plotHeight)});
        }
        Microsoft::UI::Xaml::Shapes::Polyline peakLine;
        peakLine.Points(peaks);
        peakLine.Stroke(MakeBrush(255, 184, 92, 190));
        peakLine.StrokeThickness(1.0);
        spectrumCanvas_.Children().Append(peakLine);
    } else {
        const int segmentCount = std::clamp(static_cast<int>(plotHeight / 9.0), 8, 28);
        const double stride = plotHeight / segmentCount;
        const double segmentHeight = std::max(2.0, stride - 2.0);
        for (size_t index = 0; index < count; ++index) {
            const double startT = static_cast<double>(index) / count;
            const double endT = static_cast<double>(index + 1) / count;
            const double bandLeft = frequencyToX(static_cast<float>(20.0 * std::pow(1000.0, startT)));
            const double bandRight = frequencyToX(static_cast<float>(20.0 * std::pow(1000.0, endT)));
            const double gap = std::max(1.0, (bandRight - bandLeft) / 8.0);
            const double x = bandLeft + gap;
            const double barWidth = std::max(2.0, bandRight - bandLeft - 2.0 * gap);
            const float level = std::clamp(VisualLevel(spectrumLevels_[index]) *
                                           (spectrumDisplayMode_ == SpectrumDisplayMode::LedBars ? 1.16f : 1.0f),
                                           0.0f, 1.0f);
            const float peak = std::clamp(std::max(level, VisualLevel(spectrumPeaks_[index])), 0.0f, 1.0f);

            if (spectrumDisplayMode_ == SpectrumDisplayMode::LedBars) {
                const int litSegments = std::clamp(static_cast<int>(std::ceil(level * segmentCount)), 0, segmentCount);
                const int peakSegment = std::clamp(static_cast<int>(std::ceil(peak * segmentCount)) - 1, 0, segmentCount - 1);
                for (int segment = 0; segment < segmentCount; ++segment) {
                    const double position = (static_cast<double>(segment) + 0.5) / segmentCount;
                    const bool lit = segment < litSegments;
                    const bool isPeak = segment == peakSegment && peak > 0.02f;
                    Windows::UI::Color color;
                    if (position >= 0.72) {
                        color = lit ? Rgb(255, 74, 54) : Rgb(65, 27, 29);
                    } else if (position >= 0.52) {
                        color = lit ? Rgb(255, 207, 58) : Rgb(71, 54, 23);
                    } else {
                        color = lit ? Rgb(78, 233, 115) : Rgb(20, 62, 41);
                    }
                    const double y = top + plotHeight - (segment + 1) * stride;
                    addRect(x, y, barWidth, segmentHeight, color);
                    if (isPeak) {
                        addRect(x, y, barWidth, 1.0, Rgb(255, 255, 225));
                    }
                }
            } else {
                const double barHeight = level * plotHeight;
                const double y = top + plotHeight - barHeight;
                addRect(x, y, barWidth, barHeight, Rgb(59, 188, 213));
                const double peakY = top + (1.0 - peak) * plotHeight;
                addRect(x, peakY, barWidth, 2.0, Rgb(255, 184, 92));
            }
        }
    }
}

void MainWindow::LoadSdrSettings() {
    DWORD backend = 0;
    DWORD agc = 1;
    DWORD gain = 40;
    (void)ReadRegistryDword(L"sdr_backend", backend);
    (void)ReadRegistryDword(L"sdr_agc", agc);
    (void)ReadRegistryDword(L"sdr_gain_db", gain);
    sdrSettings_.backend = backend == static_cast<DWORD>(DAB_GUI_SDR_BACKEND_RTLSDR)
                               ? DAB_GUI_SDR_BACKEND_RTLSDR
                               : DAB_GUI_SDR_BACKEND_SDRPLAY;
    sdrSettings_.agc_enabled = agc != 0 ? 1 : 0;
    sdrSettings_.gain_db = std::clamp(static_cast<int>(gain), 0, 59);
    if (sdrSettings_.backend == DAB_GUI_SDR_BACKEND_RTLSDR && sdrSettings_.gain_db > 49) {
        sdrSettings_.gain_db = 49;
    }
}

bool MainWindow::SaveSdrSettings() {
    return WriteRegistryDword(L"sdr_backend", static_cast<DWORD>(sdrSettings_.backend)) &&
           WriteRegistryDword(L"sdr_agc", static_cast<DWORD>(sdrSettings_.agc_enabled != 0)) &&
           WriteRegistryDword(L"sdr_gain_db", static_cast<DWORD>(sdrSettings_.gain_db));
}

void MainWindow::LoadSpectrumSettings() {
    DWORD mode = 0;
    (void)ReadRegistryDword(L"spectrum_display_mode", mode);
    spectrumDisplayMode_ = mode == 1 ? SpectrumDisplayMode::FftCurve
                          : mode == 2 ? SpectrumDisplayMode::LedBars
                                      : SpectrumDisplayMode::RtaBars;
}

bool MainWindow::SaveSpectrumSettings() {
    const DWORD mode = spectrumDisplayMode_ == SpectrumDisplayMode::FftCurve ? 1
                       : spectrumDisplayMode_ == SpectrumDisplayMode::LedBars ? 2
                                                                              : 0;
    return WriteRegistryDword(L"spectrum_display_mode", mode);
}

void MainWindow::ApplySdrSettings() {
    if (sdrSettings_.backend == DAB_GUI_SDR_BACKEND_RTLSDR && sdrSettings_.gain_db > 49) {
        sdrSettings_.gain_db = 49;
    }
    dab_gui_sdr_set_settings(&sdrSettings_);
}

std::string MainWindow::SdrSettingsSummary() const {
    std::ostringstream summary;
    summary << (sdrSettings_.backend == DAB_GUI_SDR_BACKEND_RTLSDR ? "RTL-SDR" : "SDRplay");
    if (sdrSettings_.agc_enabled != 0) {
        summary << ", AGC on";
    } else {
        summary << ", AGC off, gain " << sdrSettings_.gain_db << " dB";
    }
    return summary.str();
}

void MainWindow::OnSdrSettings() {
    ContentDialog dialog;
    dialog.XamlRoot(window_.Content().XamlRoot());
    dialog.Title(box_value(L"SDR settings"));
    dialog.PrimaryButtonText(L"Save");
    dialog.CloseButtonText(L"Cancel");
    dialog.DefaultButton(ContentDialogButton::Primary);

    StackPanel content;
    content.Spacing(12.0);
    content.Children().Append(MakeText(L"HAL backend", 13.0, true));
    RadioButtons backendChoice;
    backendChoice.Items().Append(box_value(L"SDRplay"));
    backendChoice.Items().Append(box_value(L"RTL-SDR"));
    backendChoice.SelectedIndex(sdrSettings_.backend == DAB_GUI_SDR_BACKEND_RTLSDR ? 1 : 0);
    content.Children().Append(backendChoice);

    CheckBox agc;
    agc.Content(box_value(L"Automatic gain control"));
    agc.IsChecked(box_value(sdrSettings_.agc_enabled != 0).as<Windows::Foundation::IReference<bool>>());
    content.Children().Append(agc);
    StackPanel gainRow;
    gainRow.Orientation(Orientation::Horizontal);
    gainRow.Spacing(10.0);
    TextBlock gainLabel = MakeText(L"Manual gain", 13.0);
    gainLabel.VerticalAlignment(VerticalAlignment::Center);
    NumberBox gain;
    gain.Minimum(0.0);
    gain.Maximum(sdrSettings_.backend == DAB_GUI_SDR_BACKEND_RTLSDR ? 49.0 : 59.0);
    gain.Value(sdrSettings_.gain_db);
    gain.SpinButtonPlacementMode(NumberBoxSpinButtonPlacementMode::Inline);
    gain.Width(140.0);
    gainRow.Children().Append(gainLabel);
    gainRow.Children().Append(gain);
    gainRow.Children().Append(MakeText(L"dB", 13.0));
    content.Children().Append(gainRow);
    dialog.Content(content);

    auto updateGain = [backendChoice, agc, gainLabel, gain]() mutable {
        const double maximum = backendChoice.SelectedIndex() == 1 ? 49.0 : 59.0;
        gain.Maximum(maximum);
        if (gain.Value() > maximum) {
            gain.Value(maximum);
        }
        const bool enabled = agc.IsChecked().Value() == false;
        gain.IsEnabled(enabled);
        gainLabel.Opacity(enabled ? 1.0 : 0.5);
    };
    agc.Checked([updateGain](IInspectable const &, RoutedEventArgs const &) mutable { updateGain(); });
    agc.Unchecked([updateGain](IInspectable const &, RoutedEventArgs const &) mutable { updateGain(); });
    backendChoice.SelectionChanged([updateGain](IInspectable const &, SelectionChangedEventArgs const &) mutable {
        updateGain();
    });
    updateGain();
    dialog.PrimaryButtonClick([this, backendChoice, agc, gain](IInspectable const &,
                                                               ContentDialogButtonClickEventArgs const &) {
        sdrSettings_.backend = backendChoice.SelectedIndex() == 1
                                   ? DAB_GUI_SDR_BACKEND_RTLSDR
                                   : DAB_GUI_SDR_BACKEND_SDRPLAY;
        sdrSettings_.agc_enabled = agc.IsChecked().Value() ? 1 : 0;
        sdrSettings_.gain_db = std::clamp(static_cast<int>(std::lround(gain.Value())), 0,
                                          sdrSettings_.backend == DAB_GUI_SDR_BACKEND_RTLSDR ? 49 : 59);
        const bool wasPlaying = dab_gui_playback_is_running() != 0;
        ApplySdrSettings();
        if (!SaveSdrSettings()) {
            controlStatusLabel_.Text(L"SDR settings were applied but could not be saved.");
        } else if (wasPlaying) {
            StopPlayback();
            controlStatusLabel_.Text(L"SDR settings saved. Restart playback to use them.");
        } else {
            controlStatusLabel_.Text(ToHString("SDR settings: " + SdrSettingsSummary()));
        }
        UpdatePlaybackControls();
    });
    (void)dialog.ShowAsync();
}

void MainWindow::OnSpectrumSettings() {
    ContentDialog dialog;
    dialog.XamlRoot(window_.Content().XamlRoot());
    dialog.Title(box_value(L"Spectrum settings"));
    dialog.PrimaryButtonText(L"Save");
    dialog.CloseButtonText(L"Cancel");
    dialog.DefaultButton(ContentDialogButton::Primary);

    StackPanel content;
    content.Spacing(8.0);
    content.Children().Append(MakeText(L"Display mode", 13.0, true));
    ComboBox mode;
    for (wchar_t const *name : {L"RTA bars", L"FFT curve", L"LED bars"}) {
        ComboBoxItem item;
        item.Content(box_value(name));
        mode.Items().Append(item);
    }
    mode.SelectedIndex(static_cast<int>(spectrumDisplayMode_));
    content.Children().Append(mode);
    dialog.Content(content);
    dialog.PrimaryButtonClick([this, mode](IInspectable const &,
                                            ContentDialogButtonClickEventArgs const &) {
        switch (mode.SelectedIndex()) {
        case 1:
            spectrumDisplayMode_ = SpectrumDisplayMode::FftCurve;
            break;
        case 2:
            spectrumDisplayMode_ = SpectrumDisplayMode::LedBars;
            break;
        default:
            spectrumDisplayMode_ = SpectrumDisplayMode::RtaBars;
            break;
        }
        if (!SaveSpectrumSettings()) {
            controlStatusLabel_.Text(L"Spectrum mode changed but could not be saved.");
        }
        DrawSpectrum();
    });
    (void)dialog.ShowAsync();
}

void MainWindow::PushEqualizerToBackend() {
    std::array<float, DAB_AUDIO_EQ_BAND_COUNT> gains{};
    for (size_t index = 0; index < std::min(gains.size(), equalizerSliders_.size()); ++index) {
        gains[index] = static_cast<float>(equalizerSliders_[index].Value());
    }
    if (dab_gui_playback_set_equalizer(gains.data(), static_cast<uint32_t>(gains.size())) != 0) {
        controlStatusLabel_.Text(L"Could not update the playback equalizer.");
    }
}

void MainWindow::ApplyEqualizerPreset(size_t index) {
    if (index >= kEqualizerPresets.size()) {
        return;
    }
    applyingEqualizerPreset_ = true;
    const auto &preset = kEqualizerPresets[index];
    for (size_t band = 0; band < equalizerSliders_.size(); ++band) {
        equalizerSliders_[band].Value(preset.gains[band]);
        equalizerValueLabels_[band].Text(FormatDb(preset.gains[band]));
    }
    equalizerPresetDescription_.Text(preset.description);
    equalizerPresetChoice_.SelectedIndex(static_cast<int>(index));
    applyingEqualizerPreset_ = false;
    PushEqualizerToBackend();
}

void MainWindow::OnScanEvent(const char *line, void *user) {
    if (line == nullptr || user == nullptr) {
        return;
    }
    auto *self = static_cast<MainWindow *>(user);
    std::lock_guard<std::mutex> lock(self->scanLinesMutex_);
    if (self->scanCallbacksEnabled_) {
        self->pendingScanLines_.emplace_back(line);
    }
}

void MainWindow::StartScan() {
    if (scanResultsPath_.empty()) {
        controlStatusLabel_.Text(L"Set a writable local application data folder before scanning.");
        return;
    }
    if (dab_gui_playback_is_running() != 0) {
        StopPlayback();
    }
    if (scanWorker_.joinable()) {
        scanWorker_.join();
    }
    {
        std::lock_guard<std::mutex> lock(scanLinesMutex_);
        scanCallbacksEnabled_ = true;
        pendingScanLines_.clear();
    }
    scanSucceeded_ = false;
    scanFinished_.store(false);
    scanResultCode_.store(-1);
    scanProgress_.Maximum(1.0);
    scanProgress_.Value(0.0);
    dab_gui_sdr_settings_t settings{};
    dab_gui_sdr_get_settings(&settings);
    scanStatusLabel_.Text(settings.backend == DAB_GUI_SDR_BACKEND_RTLSDR
                              ? L"Opening RTL-SDR backend..."
                              : L"Opening SDRplay backend...");
    scanChannelLabel_.Text(L"Channel: --");
    scanPanel_.Visibility(Visibility::Visible);
    scanButton_.Content(box_value(L"Cancel scan"));
    scanCancelButton_.Content(box_value(L"Cancel scan"));
    controlStatusLabel_.Text(L"Scanning DAB channels...");
    const std::string outputPath = winrt::to_string(hstring(scanResultsPath_));
    scanWorker_ = std::thread([this, outputPath]() {
        const int result = dab_gui_scan_run(outputPath.c_str(), &MainWindow::OnScanEvent, this);
        scanResultCode_.store(result);
        scanFinished_.store(true);
    });
}

void MainWindow::CancelScan() {
    if (scanWorker_.joinable() && !scanFinished_.load()) {
        scanStatusLabel_.Text(L"Cancelling scan...");
        scanCancelButton_.IsEnabled(false);
        scanButton_.IsEnabled(false);
        dab_gui_scan_cancel_current();
        return;
    }
    scanPanel_.Visibility(Visibility::Collapsed);
    scanCancelButton_.IsEnabled(true);
    scanButton_.IsEnabled(true);
    scanButton_.Content(box_value(L"Scan"));
}

void MainWindow::StopScanWorker() {
    if (scanWorker_.joinable()) {
        if (!scanFinished_.load()) {
            dab_gui_scan_cancel_current();
        }
        {
            std::lock_guard<std::mutex> lock(scanLinesMutex_);
            scanCallbacksEnabled_ = false;
        }
        scanWorker_.join();
    }
    scanFinished_.store(true);
}

void MainWindow::DrainScanEvents() {
    std::deque<std::string> lines;
    {
        std::lock_guard<std::mutex> lock(scanLinesMutex_);
        lines.swap(pendingScanLines_);
    }
    for (const std::string &line : lines) {
        HandleScanLine(line);
    }
    if (scanWorker_.joinable() && scanFinished_.load()) {
        FinalizeScan();
    }
}

void MainWindow::HandleScanLine(std::string const &line) {
    const std::vector<std::string> fields = SplitTabs(line);
    if (fields.empty()) {
        return;
    }
    if (fields[0] == "SCAN_PROGRESS" && fields.size() >= 5) {
        int index = 0;
        int total = 0;
        (void)std::from_chars(fields[1].data(), fields[1].data() + fields[1].size(), index);
        (void)std::from_chars(fields[2].data(), fields[2].data() + fields[2].size(), total);
        const double range = static_cast<double>(std::max(1, total));
        scanProgress_.Maximum(range);
        scanProgress_.Value(std::clamp(static_cast<double>(index), 0.0, range));
        scanChannelLabel_.Text(ToHString("Channel: " + fields[3]));
        scanStatusLabel_.Text(ToHString("Scanning " + fields[3] + " (" +
                                         std::to_string(index) + " of " +
                                         std::to_string(total) + ")"));
    } else if (fields[0] == "SCAN_LOCK" && fields.size() >= 7) {
        scanChannelLabel_.Text(ToHString("Channel: " + fields[3]));
        scanStatusLabel_.Text(ToHString("Locked on " + fields[3] +
                                         ", services=" + fields[5] +
                                         ", labels=" + fields[6]));
    } else if (fields[0] == "SCAN_COMPLETE" && fields.size() >= 3) {
        scanProgress_.Value(scanProgress_.Maximum());
        scanStatusLabel_.Text(ToHString("Scan complete. Services found: " + fields[2]));
    } else if (fields[0] == "SCAN_ERROR" && fields.size() >= 2) {
        scanStatusLabel_.Text(ToHString(fields[1]));
    }
}

void MainWindow::FinalizeScan() {
    if (scanWorker_.joinable()) {
        scanWorker_.join();
    }
    {
        std::lock_guard<std::mutex> lock(scanLinesMutex_);
        scanCallbacksEnabled_ = false;
    }
    const int result = scanResultCode_.load();
    scanSucceeded_ = result == 0;
    if (scanSucceeded_) {
        scanStatusLabel_.Text(L"Scan complete.");
        scanChannelLabel_.Text(ToHString("Results saved to " + winrt::to_string(hstring(scanResultsPath_))));
        controlStatusLabel_.Text(L"Scan complete. Select a station and press Start.");
        ReloadStationsFromScanResults();
    } else if (result == -2) {
        scanStatusLabel_.Text(L"Scan cancelled.");
        controlStatusLabel_.Text(L"Station scan cancelled.");
    } else {
        scanStatusLabel_.Text(L"Scan failed before completion.");
        controlStatusLabel_.Text(L"Station scan failed.");
    }
    scanCancelButton_.Content(box_value(L"Close"));
    scanCancelButton_.IsEnabled(true);
    scanButton_.IsEnabled(true);
    scanButton_.Content(box_value(L"Scan"));
    UpdatePlaybackControls();
}

void MainWindow::ReloadStationsFromScanResults() {
    if (!stationChoice_ || scanResultsPath_.empty()) {
        return;
    }
    std::ifstream file(std::filesystem::path(scanResultsPath_), std::ios::binary);
    if (!file) {
        return;
    }

    stationEntries_.clear();
    stationChoice_.Items().Clear();
    std::string line;
    if (!std::getline(file, line)) {
        return;
    }
    while (std::getline(file, line)) {
        const std::vector<std::string> fields = SplitTabs(line);
        if (fields.size() < 4) {
            continue;
        }
        const std::string frequencyText = Trim(fields[1]);
        uint64_t frequency = 0;
        const auto frequencyResult = std::from_chars(frequencyText.data(),
                                                      frequencyText.data() + frequencyText.size(),
                                                      frequency);
        uint32_t serviceId = 0;
        if (frequencyResult.ec != std::errc{} ||
            frequencyResult.ptr != frequencyText.data() + frequencyText.size() ||
            !ParseServiceId(Trim(fields[2]), serviceId)) {
            continue;
        }
        std::string label = Trim(fields[3]);
        if (label.empty()) {
            char fallback[16]{};
            sprintf_s(fallback, "0x%08X", serviceId);
            label = fallback;
        }
        stationEntries_.push_back({Trim(fields[0]), label, frequency, serviceId});
        ComboBoxItem item;
        item.Content(box_value(ToHString(label)));
        stationChoice_.Items().Append(item);
    }

    if (!stationEntries_.empty()) {
        stationChoice_.SelectedIndex(0);
        if (dab_gui_playback_is_running() == 0) {
            controlStatusLabel_.Text(ToHString("Loaded " + std::to_string(stationEntries_.size()) +
                                               " station(s). Select one and press Start."));
        }
    } else if (dab_gui_playback_is_running() == 0) {
        controlStatusLabel_.Text(L"Scan finished but no stations were found.");
    }
    UpdatePlaybackControls();
}

void MainWindow::UpdatePlaybackControls() {
    const int selection = stationChoice_ ? stationChoice_.SelectedIndex() : -1;
    const bool hasSelection = selection >= 0 && static_cast<size_t>(selection) < stationEntries_.size();
    const bool running = dab_gui_playback_is_running() != 0;
    startButton_.IsEnabled(hasSelection && !running);
    stopButton_.IsEnabled(running);
}

void MainWindow::StartPlaybackForCurrentSelection() {
    const int selection = stationChoice_ ? stationChoice_.SelectedIndex() : -1;
    if (dab_gui_playback_is_running() != 0) {
        controlStatusLabel_.Text(L"Playback is already running.");
        UpdatePlaybackControls();
        return;
    }
    if (selection < 0 || static_cast<size_t>(selection) >= stationEntries_.size()) {
        controlStatusLabel_.Text(L"Select a station first.");
        UpdatePlaybackControls();
        return;
    }
    const StationEntry &station = stationEntries_[static_cast<size_t>(selection)];
    if (dab_gui_playback_start(station.frequencyHz, station.serviceId) != 0) {
        controlStatusLabel_.Text(L"Failed to start playback.");
    } else {
        controlStatusLabel_.Text(ToHString("Playing " + station.label + " on " + station.channel));
    }
    UpdatePlaybackControls();
}

void MainWindow::StopPlayback() {
    if (dab_gui_playback_is_running() == 0) {
        controlStatusLabel_.Text(L"Playback is not running.");
        UpdatePlaybackControls();
        return;
    }
    controlStatusLabel_.Text(L"Stopping playback...");
    dab_gui_playback_stop();
    ClearLogo();
    controlStatusLabel_.Text(L"Playback stopped.");
    UpdatePlaybackControls();
}

void MainWindow::RefreshPlaybackStatus() {
    currentTimeLabel_.Text(CurrentSystemTime());
    dab_gui_playback_status_t status{};
    if (dab_gui_playback_get_status(&status) != 0) {
        controlStatusLabel_.Text(L"Could not read playback status from the receiver.");
        return;
    }

    ensembleLabel_.Text(status.has_ensemble_label
                            ? ToHString("Ensemble: " + std::string(status.ensemble_label))
                            : L"Ensemble: -");
    if (status.has_ensemble_time) {
        std::ostringstream time;
        time << ((status.ensemble_time_utc != 0) ? "Ensemble UTC: " : "Ensemble local: ")
             << std::setfill('0') << std::setw(2) << static_cast<unsigned>(status.ensemble_time_hour)
             << ':' << std::setw(2) << static_cast<unsigned>(status.ensemble_time_minute);
        if (status.ensemble_time_has_seconds != 0) {
            time << ':' << std::setw(2) << static_cast<unsigned>(status.ensemble_time_second);
        }
        ensembleTimeLabel_.Text(ToHString(time.str()));
    } else {
        ensembleTimeLabel_.Text(L"Ensemble: --:--");
    }

    if (!status.running) {
        playbackWasRunning_ = false;
        ensembleLabel_.Text(L"Ensemble: -");
        ensembleTimeLabel_.Text(L"Ensemble: --:--");
        nowPlayingLabel_.Text(L"Now playing: -");
        if (receiverStatusLabel_) {
            receiverStatusLabel_.Text(L"Receiver: idle");
        }
        ClearLogo();
        UpdatePlaybackControls();
        return;
    }

    if (!playbackWasRunning_) {
        playbackStartedAt_ = std::chrono::steady_clock::now();
        playbackWasRunning_ = true;
    }
    const std::string nowPlaying(status.now_playing);
    if (nowPlaying.empty()) {
        const auto elapsed = std::chrono::steady_clock::now() - playbackStartedAt_;
        nowPlayingLabel_.Text(elapsed >= std::chrono::seconds(10)
                                  ? L"Now playing: No song text received"
                                  : L"Now playing: Waiting for station text...");
    } else {
        nowPlayingLabel_.Text(ToHString("Now playing: " + nowPlaying));
    }

    dab_gui_playback_logo_t logo{};
    if (dab_gui_playback_get_logo(&logo) != 0) {
        controlStatusLabel_.Text(L"Could not read the station logo from the receiver.");
    } else if (logo.valid != 0 && logo.byte_count > 0 &&
               logo.generation != lastLogoGeneration_ &&
               logo.generation != pendingLogoGeneration_) {
        pendingLogoGeneration_ = logo.generation;
        LoadLogoAsync(std::vector<uint8_t>(logo.bytes, logo.bytes + logo.byte_count), logo.generation);
    } else if (logo.valid == 0 && (logoVisible_ || pendingLogoGeneration_ != 0)) {
        ClearLogo();
    }

    if (receiverStatusLabel_) {
        receiverStatusLabel_.Text(ToHString(FormatReceiverStatus(status)));
    }
    UpdatePlaybackControls();
}

void MainWindow::ClearLogo() {
    stationLogo_.Source(nullptr);
    stationLogo_.Visibility(Visibility::Collapsed);
    logoVisible_ = false;
    lastLogoGeneration_ = 0;
    pendingLogoGeneration_ = 0;
}

fire_and_forget MainWindow::LoadLogoAsync(std::vector<uint8_t> bytes, uint64_t generation) {
    const std::weak_ptr<MainWindow> weak = weak_from_this();
    try {
        InMemoryRandomAccessStream stream;
        DataWriter writer(stream.GetOutputStreamAt(0));
        writer.WriteBytes(array_view<uint8_t const>(bytes));
        (void)co_await writer.StoreAsync();
        (void)co_await writer.FlushAsync();
        writer.DetachStream();
        stream.Seek(0);

        BitmapImage image;
        co_await image.SetSourceAsync(stream);
        if (const auto self = weak.lock(); self && self->pendingLogoGeneration_ == generation) {
            self->stationLogo_.Source(image);
            self->stationLogo_.Visibility(Visibility::Visible);
            self->logoVisible_ = true;
            self->lastLogoGeneration_ = generation;
            self->pendingLogoGeneration_ = 0;
        }
    } catch (hresult_error const &error) {
        if (const auto self = weak.lock(); self && self->pendingLogoGeneration_ == generation) {
            self->pendingLogoGeneration_ = 0;
            self->controlStatusLabel_.Text(L"Could not decode the station logo: " + error.message());
        }
    }
}
