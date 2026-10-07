#ifndef DAB_GUI_MAIN_FRAME_HPP
#define DAB_GUI_MAIN_FRAME_HPP

#include <chrono>
#include <vector>

#include <wx/frame.h>
#include <wx/string.h>

#include "dab_gui_backend.h"

class wxChoice;
class wxCommandEvent;
class wxTimerEvent;
class wxTimer;
class wxButton;
class wxStaticText;
class wxWindow;

class MainFrame : public wxFrame {
public:
    explicit MainFrame(bool showEnsembleTime);

private:
    struct StationEntry {
        wxString channel;
        wxString label;
        wxString serviceId;
        unsigned long long frequencyHz;
    };

    void OnScanButton(wxCommandEvent &event);
    void OnStartButton(wxCommandEvent &event);
    void OnStopButton(wxCommandEvent &event);
    void OnStationSelected(wxCommandEvent &event);
    void OnSdrSettings(wxCommandEvent &event);
    void OnSpectrumSettings(wxCommandEvent &event);
    wxWindow *CreateBottomPanel(wxWindow *parent, bool showEnsembleTime);
    void LoadSdrSettings();
    void SaveSdrSettings();
    void LoadSpectrumSettings();
    void SaveSpectrumSettings();
    void ApplySdrSettings();
    wxString SdrSettingsSummary() const;
    void ReloadStationsFromScanResults();
    void UpdatePlaybackControls();
    void StartPlaybackForCurrentSelection();
    void StopPlayback();
    void OnPlaybackStatusTimer(wxTimerEvent &event);
    void RefreshPlaybackStatus();

    wxChoice *stationChoice_ = nullptr;
    wxButton *startButton_ = nullptr;
    wxButton *stopButton_ = nullptr;
    wxStaticText *ensembleLabel_ = nullptr;
    wxStaticText *currentTimeLabel_ = nullptr;
    wxStaticText *ensembleTimeLabel_ = nullptr;
    wxStaticText *nowPlayingLabel_ = nullptr;
    wxStaticText *controlStatusLabel_ = nullptr;
    wxStaticText *receiverStatusLabel_ = nullptr;
    wxWindow *stationLogoPanel_ = nullptr;
    wxString scanResultsPath_;
    std::vector<StationEntry> stationEntries_;
    wxTimer *playbackStatusTimer_ = nullptr;
    std::chrono::steady_clock::time_point playbackStartedAt_;
    bool playbackWasRunning_ = false;
    uint64_t lastLogoGeneration_ = 0u;
    dab_gui_sdr_settings_t sdrSettings_;
    int spectrumDisplayMode_ = 0;
};

#endif
