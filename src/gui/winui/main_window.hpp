#ifndef DAB_GUI_WINUI_MAIN_WINDOW_HPP
#define DAB_GUI_WINUI_MAIN_WINDOW_HPP

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <winrt/Microsoft.UI.Xaml.h>
#include <winrt/Microsoft.UI.Xaml.Controls.h>
#include <winrt/Microsoft.UI.Xaml.Shapes.h>

#include "dab_gui_backend.h"

class MainWindow : public std::enable_shared_from_this<MainWindow> {
public:
    explicit MainWindow(bool showEnsembleTime);
    ~MainWindow();

    void Activate();

private:
    struct StationEntry {
        std::string channel;
        std::string label;
        uint64_t frequencyHz = 0;
        uint32_t serviceId = 0;
    };

    enum class SpectrumDisplayMode {
        RtaBars,
        FftCurve,
        LedBars
    };

    void BuildUi(bool showEnsembleTime);
    void BuildSpectrumPanel(winrt::Microsoft::UI::Xaml::Controls::StackPanel const &parent);
    void BuildEqualizerPanel(winrt::Microsoft::UI::Xaml::Controls::StackPanel const &parent);
    void BuildPlaybackPanel(winrt::Microsoft::UI::Xaml::Controls::StackPanel const &parent);
    void BuildScanPanel(winrt::Microsoft::UI::Xaml::Controls::StackPanel const &parent);
    void DrawSpectrum();
    void RefreshPlaybackStatus();
    void DrainScanEvents();
    void HandleScanLine(std::string const &line);
    void FinalizeScan();
    void ReloadStationsFromScanResults();
    void UpdatePlaybackControls();
    void StartPlaybackForCurrentSelection();
    void StopPlayback();
    void StartScan();
    void CancelScan();
    void StopScanWorker();
    void OnSdrSettings();
    void OnSpectrumSettings();
    void LoadSdrSettings();
    bool SaveSdrSettings();
    void LoadSpectrumSettings();
    bool SaveSpectrumSettings();
    void ApplySdrSettings();
    std::string SdrSettingsSummary() const;
    void PushEqualizerToBackend();
    void ApplyEqualizerPreset(size_t index);
    void ClearLogo();
    winrt::fire_and_forget LoadLogoAsync(std::vector<uint8_t> bytes, uint64_t generation);
    static void OnScanEvent(const char *line, void *user);

    winrt::Microsoft::UI::Xaml::Window window_{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::Canvas spectrumCanvas_{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::Image stationLogo_{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::ComboBox stationChoice_{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::ComboBox equalizerPresetChoice_{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::TextBlock equalizerPresetDescription_{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::Button startButton_{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::Button stopButton_{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::Button scanButton_{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::Button scanCancelButton_{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::TextBlock ensembleLabel_{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::TextBlock ensembleTimeLabel_{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::TextBlock currentTimeLabel_{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::TextBlock nowPlayingLabel_{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::TextBlock controlStatusLabel_{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::TextBlock receiverStatusLabel_{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::TextBlock scanStatusLabel_{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::TextBlock scanChannelLabel_{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::ProgressBar scanProgress_{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::Border scanPanel_{nullptr};
    winrt::Microsoft::UI::Xaml::DispatcherTimer statusTimer_{nullptr};
    winrt::Microsoft::UI::Xaml::DispatcherTimer spectrumTimer_{nullptr};
    std::vector<winrt::Microsoft::UI::Xaml::Controls::Slider> equalizerSliders_;
    std::vector<winrt::Microsoft::UI::Xaml::Controls::TextBlock> equalizerValueLabels_;
    std::vector<StationEntry> stationEntries_;
    std::array<float, DAB_AUDIO_SPECTRUM_DISPLAY_BINS> spectrumLevels_{};
    std::array<float, DAB_AUDIO_SPECTRUM_DISPLAY_BINS> spectrumPeaks_{};
    std::wstring scanResultsPath_;
    std::thread scanWorker_;
    std::mutex scanLinesMutex_;
    std::deque<std::string> pendingScanLines_;
    std::atomic<bool> scanFinished_{false};
    std::atomic<int> scanResultCode_{-1};
    bool scanCallbacksEnabled_ = true;
    bool scanSucceeded_ = false;
    bool showEnsembleTime_ = false;
    bool showReceiverStatus_ = false;
    bool playbackWasRunning_ = false;
    bool applyingEqualizerPreset_ = false;
    bool logoVisible_ = false;
    uint64_t lastLogoGeneration_ = 0;
    uint64_t pendingLogoGeneration_ = 0;
    std::chrono::steady_clock::time_point playbackStartedAt_{};
    dab_gui_sdr_settings_t sdrSettings_{};
    SpectrumDisplayMode spectrumDisplayMode_ = SpectrumDisplayMode::RtaBars;
};

#endif
