#ifndef DAB_GUI_MAIN_FRAME_HPP
#define DAB_GUI_MAIN_FRAME_HPP

#include <vector>

#include <wx/frame.h>
#include <wx/string.h>

class wxChoice;
class wxCommandEvent;
class wxTimerEvent;
class wxButton;
class wxStaticText;
class wxWindow;

class MainFrame : public wxFrame {
public:
    MainFrame();

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
    wxWindow *CreateBottomPanel(wxWindow *parent);
    void ReloadStationsFromScanResults();
    void UpdatePlaybackControls();
    void StartPlaybackForCurrentSelection();
    void StopPlayback();

    wxChoice *stationChoice_ = nullptr;
    wxButton *startButton_ = nullptr;
    wxButton *stopButton_ = nullptr;
    wxStaticText *controlStatusLabel_ = nullptr;
    wxString scanResultsPath_;
    std::vector<StationEntry> stationEntries_;
};

#endif
