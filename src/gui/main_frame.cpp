#include "main_frame.hpp"
#include "dab_gui_backend.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <wx/button.h>
#include <wx/choice.h>
#include <wx/dialog.h>
#include <wx/dcbuffer.h>
#include <wx/dcgraph.h>
#include <wx/filename.h>
#include <wx/gauge.h>
#include <wx/panel.h>
#include <wx/settings.h>
#include <wx/sizer.h>
#include <wx/slider.h>
#include <wx/stdpaths.h>
#include <wx/stattext.h>
#include <wx/timer.h>
#include <wx/tokenzr.h>
#include <wx/utils.h>

namespace {

constexpr int kEqualizerTimerId = wxID_HIGHEST + 1;
constexpr int kScanTimerId = wxID_HIGHEST + 2;
constexpr double kTwoPi = 6.28318530717958647692;

class SpectrumPanel : public wxPanel {
public:
    explicit SpectrumPanel(wxWindow *parent)
        : wxPanel(parent),
          phases_{0.15, 0.38, 0.61, 0.84, 1.07, 1.30, 1.53, 1.76, 1.99, 2.22, 2.45, 2.68},
          timer_(this, kEqualizerTimerId) {
        SetMinSize(wxSize(-1, 190));
        SetBackgroundStyle(wxBG_STYLE_PAINT);

        Bind(wxEVT_PAINT, &SpectrumPanel::OnPaint, this);
        Bind(wxEVT_TIMER, &SpectrumPanel::OnTimer, this, kEqualizerTimerId);
        timer_.Start(80);
    }

private:
    void OnTimer(wxTimerEvent &) {
        for (double &phase : phases_) {
            phase += 0.11;
            if (phase > kTwoPi) {
                phase -= kTwoPi;
            }
        }
        Refresh(false);
    }

    void OnPaint(wxPaintEvent &) {
        wxAutoBufferedPaintDC dc(this);
        wxGCDC gcdc(dc);
        wxDC &draw = gcdc.IsOk() ? static_cast<wxDC &>(gcdc) : static_cast<wxDC &>(dc);

        const wxRect rect = GetClientRect();
        draw.SetBackground(wxBrush(wxColour(18, 22, 28)));
        draw.Clear();

        draw.SetPen(wxPen(wxColour(42, 52, 64), 1));
        for (int row = 1; row < 5; ++row) {
            const int y = rect.y + (rect.height * row) / 5;
            draw.DrawLine(rect.x + 12, y, rect.GetRight() - 12, y);
        }

        const int bars = static_cast<int>(phases_.size());
        const int left = rect.x + 18;
        const int top = rect.y + 16;
        const int height = std::max(1, rect.height - 32);
        const int width = std::max(1, rect.width - 36);
        const int gap = 8;
        const int barWidth = std::max(10, (width - gap * (bars - 1)) / bars);

        for (int i = 0; i < bars; ++i) {
            const double harmonic = std::sin(phases_[i]) * 0.5 + 0.5;
            const double accent = std::sin((phases_[i] * 2.0) + (i * 0.33)) * 0.15 + 0.15;
            const double level = std::clamp(0.22 + harmonic * 0.55 + accent, 0.12, 0.96);
            const int barHeight = static_cast<int>(level * height);
            const int x = left + i * (barWidth + gap);
            const int y = top + height - barHeight;

            const wxColour fill(36 + i * 10, 170 + (i % 3) * 16, 196 - i * 7);
            draw.SetPen(*wxTRANSPARENT_PEN);
            draw.SetBrush(wxBrush(fill));
            draw.DrawRoundedRectangle(x, y, barWidth, barHeight, 4);
        }

        draw.SetTextForeground(wxColour(180, 194, 210));
        draw.DrawText("Graphical equalizer", rect.x + 16, rect.y + 10);
    }

    std::array<double, 12> phases_;
    wxTimer timer_;
};

class StationLogoPanel : public wxPanel {
public:
    explicit StationLogoPanel(wxWindow *parent) : wxPanel(parent) {
        SetMinSize(wxSize(220, 110));
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        Bind(wxEVT_PAINT, &StationLogoPanel::OnPaint, this);
    }

private:
    void OnPaint(wxPaintEvent &) {
        wxAutoBufferedPaintDC dc(this);
        wxGCDC gcdc(dc);
        wxDC &draw = gcdc.IsOk() ? static_cast<wxDC &>(gcdc) : static_cast<wxDC &>(dc);

        const wxRect rect = GetClientRect();
        draw.SetBackground(wxBrush(wxColour(28, 33, 40)));
        draw.Clear();

        wxRect inset(rect);
        inset.Deflate(12, 12);
        draw.SetPen(wxPen(wxColour(70, 82, 96), 2));
        draw.SetBrush(wxBrush(wxColour(44, 132, 188)));
        draw.DrawRoundedRectangle(inset, 12);

        draw.SetTextForeground(*wxWHITE);
        wxFont titleFont = GetFont();
        titleFont.SetPointSize(titleFont.GetPointSize() + 8);
        titleFont.SetWeight(wxFONTWEIGHT_BOLD);
        draw.SetFont(titleFont);
        draw.DrawText("DAB", inset.x + 18, inset.y + 16);

        wxFont bodyFont = GetFont();
        bodyFont.SetPointSize(bodyFont.GetPointSize() + 1);
        draw.SetFont(bodyFont);
        draw.DrawText("Station logo", inset.x + 18, inset.y + 58);
        draw.DrawText("placeholder", inset.x + 18, inset.y + 80);
    }
};

class EqualizerPanel : public wxPanel {
public:
    explicit EqualizerPanel(wxWindow *parent) : wxPanel(parent) {
        SetMinSize(wxSize(-1, 220));
        SetBackgroundColour(wxColour(236, 240, 244));

        auto *outer = new wxBoxSizer(wxVERTICAL);
        auto *header = new wxBoxSizer(wxHORIZONTAL);

        auto *title = new wxStaticText(this, wxID_ANY, "Equalizer bands");
        wxFont titleFont = title->GetFont();
        titleFont.SetWeight(wxFONTWEIGHT_BOLD);
        title->SetFont(titleFont);

        auto *presetLabel = new wxStaticText(this, wxID_ANY, "Preset");
        presetChoice_ = new wxChoice(this, wxID_ANY);
        for (const auto &preset : Presets()) {
            presetChoice_->Append(preset.name);
        }
        presetChoice_->SetSelection(0);

        presetStatus_ = new wxStaticText(this, wxID_ANY, "Balanced response for general listening");
        presetStatus_->SetForegroundColour(wxColour(95, 104, 116));

        header->Add(title, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 18);
        header->AddStretchSpacer(1);
        header->Add(presetLabel, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);
        header->Add(presetChoice_, 0, wxALIGN_CENTER_VERTICAL);

        outer->Add(header, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 12);
        outer->Add(presetStatus_, 0, wxLEFT | wxRIGHT | wxTOP, 12);

        auto *sliders = new wxBoxSizer(wxHORIZONTAL);
        for (size_t i = 0; i < kBands.size(); ++i) {
            auto *column = new wxBoxSizer(wxVERTICAL);
            auto *valueLabel = new wxStaticText(this, wxID_ANY, " ");
            auto *slider = new wxSlider(
                this,
                wxID_ANY,
                0,
                -12,
                12,
                wxDefaultPosition,
                wxSize(44, 150),
                wxSL_VERTICAL | wxSL_INVERSE | wxSL_AUTOTICKS
            );
            auto *bandLabel = new wxStaticText(this, wxID_ANY, kBands[i]);

            slider->SetTickFreq(3);
            valueLabel->SetLabel(wxString::Format("%+d dB", slider->GetValue()));
            valueLabels_.push_back(valueLabel);
            sliders_.push_back(slider);

            slider->Bind(wxEVT_SLIDER, [this, valueLabel](wxCommandEvent &event) {
                valueLabel->SetLabel(wxString::Format("%+d dB", event.GetInt()));
                if (!applyingPreset_) {
                    presetChoice_->SetStringSelection("Custom");
                    presetStatus_->SetLabel("Manual band adjustment");
                }
            });

            column->Add(valueLabel, 0, wxALIGN_CENTER | wxBOTTOM, 6);
            column->Add(slider, 1, wxALIGN_CENTER | wxBOTTOM, 8);
            column->Add(bandLabel, 0, wxALIGN_CENTER);
            sliders->Add(column, 1, wxEXPAND | wxALL, 8);
        }

        outer->Add(sliders, 1, wxEXPAND | wxALL, 8);
        SetSizer(outer);

        presetChoice_->Bind(wxEVT_CHOICE, &EqualizerPanel::OnPresetSelected, this);
        ApplyPreset(0);
    }

private:
    struct PresetDefinition {
        const char *name;
        const char *description;
        std::array<int, 10> gains;
    };

    static const std::array<PresetDefinition, 7> &Presets() {
        static const std::array<PresetDefinition, 7> presets = {{
            {"Flat", "Balanced response for general listening", {0, 0, 0, 0, 0, 0, 0, 0, 0, 0}},
            {"Rock", "Extra punch in bass and treble", {5, 4, 2, 0, -1, 1, 3, 4, 5, 4}},
            {"Pop", "Forward vocals with bright upper mids", {2, 3, 4, 3, 1, 0, 2, 3, 4, 3}},
            {"Jazz", "Warm low end and open detail", {3, 2, 1, 0, 1, 2, 3, 2, 1, 1}},
            {"Classical", "Light bass lift and airy highs", {1, 1, 0, -1, 0, 2, 3, 4, 4, 3}},
            {"Electronic", "Deep bass with crisp top end", {6, 5, 3, 1, 0, 2, 4, 5, 5, 4}},
            {"Custom", "Manual band adjustment", {0, 0, 0, 0, 0, 0, 0, 0, 0, 0}},
        }};
        return presets;
    }

    void OnPresetSelected(wxCommandEvent &event) {
        ApplyPreset(static_cast<size_t>(event.GetSelection()));
    }

    void ApplyPreset(size_t presetIndex) {
        const auto &preset = Presets()[presetIndex];
        applyingPreset_ = true;
        for (size_t i = 0; i < sliders_.size(); ++i) {
            sliders_[i]->SetValue(preset.gains[i]);
            valueLabels_[i]->SetLabel(wxString::Format("%+d dB", preset.gains[i]));
        }
        applyingPreset_ = false;
        presetStatus_->SetLabel(preset.description);
        Layout();
    }

    static constexpr std::array<const char *, 10> kBands = {
        "60 Hz", "170 Hz", "310 Hz", "600 Hz", "1 kHz",
        "3 kHz", "6 kHz", "10 kHz", "12 kHz", "16 kHz"
    };

    wxChoice *presetChoice_ = nullptr;
    wxStaticText *presetStatus_ = nullptr;
    std::vector<wxSlider *> sliders_;
    std::vector<wxStaticText *> valueLabels_;
    bool applyingPreset_ = false;
};

class ScanDialog : public wxDialog {
public:
    explicit ScanDialog(wxWindow *parent, wxString resultsPath)
        : wxDialog(parent, wxID_ANY, "Scanning DAB Channels", wxDefaultPosition, wxSize(460, 220),
                   wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER),
          resultsPath_(std::move(resultsPath)),
          pollTimer_(this, kScanTimerId) {
        auto *outer = new wxBoxSizer(wxVERTICAL);

        auto *title = new wxStaticText(this, wxID_ANY, "DAB channel scan in progress");
        wxFont titleFont = title->GetFont();
        titleFont.SetWeight(wxFONTWEIGHT_BOLD);
        titleFont.SetPointSize(titleFont.GetPointSize() + 1);
        title->SetFont(titleFont);

        statusLabel_ = new wxStaticText(this, wxID_ANY, "Preparing tuner...");
        channelLabel_ = new wxStaticText(this, wxID_ANY, "Channel: --");
        progress_ = new wxGauge(this, wxID_ANY, 1);
        progress_->SetValue(0);

        closeButton_ = new wxButton(this, wxID_OK, "Done");
        closeButton_->Enable(false);

        outer->Add(title, 0, wxALL, 16);
        outer->Add(statusLabel_, 0, wxLEFT | wxRIGHT | wxBOTTOM, 16);
        outer->Add(channelLabel_, 0, wxLEFT | wxRIGHT | wxBOTTOM, 12);
        outer->Add(progress_, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 16);
        outer->Add(closeButton_, 0, wxALIGN_RIGHT | wxLEFT | wxRIGHT | wxBOTTOM, 16);

        SetSizerAndFit(outer);
        SetMinSize(wxSize(420, 200));
        CentreOnParent();

        Bind(wxEVT_TIMER, &ScanDialog::OnPollTimer, this, kScanTimerId);
        StartScan();
    }

    ~ScanDialog() override {
        if (workerThread_.joinable()) {
            workerThread_.join();
        }
    }

    bool ScanSucceeded() const {
        return scanSucceeded_;
    }

private:
    static void OnScanEvent(const char *line, void *user) {
        auto *dialog = static_cast<ScanDialog *>(user);
        std::lock_guard<std::mutex> lock(dialog->linesMutex_);
        dialog->pendingLines_.push_back(wxString::FromUTF8(line));
    }

    void StartScan() {
        statusLabel_->SetLabel("Opening SDRplay backend...");
        resultsPathUtf8_ = resultsPath_.ToStdString();
        scanFinished_.store(false);
        scanResultCode_.store(-1);
        workerThread_ = std::thread([this]() {
            const int result = dab_gui_scan_run(resultsPathUtf8_.c_str(), &ScanDialog::OnScanEvent, this);
            scanResultCode_.store(result);
            scanFinished_.store(true);
        });
        pollTimer_.Start(120);
    }

    void OnPollTimer(wxTimerEvent &) {
        std::deque<wxString> lines;

        {
            std::lock_guard<std::mutex> lock(linesMutex_);
            lines.swap(pendingLines_);
        }
        for (const wxString &line : lines) {
            HandleProgressLine(line);
        }

        if (scanFinished_.load()) {
            pollTimer_.Stop();
            if (workerThread_.joinable()) {
                workerThread_.join();
            }
            scanSucceeded_ = scanResultCode_.load() == 0;
            if (scanSucceeded_) {
                statusLabel_->SetLabel("Scan complete.");
                channelLabel_->SetLabel(wxString::Format("Results saved to %s", resultsPath_));
            } else {
                statusLabel_->SetLabel("Scan failed before completion.");
            }
            closeButton_->SetLabel(scanSucceeded_ ? "Done" : "Close");
            closeButton_->Enable(true);
        }
    }

    void HandleProgressLine(const wxString &line) {
        wxStringTokenizer tokenizer(line, "\t", wxTOKEN_RET_EMPTY_ALL);
        std::vector<wxString> parts;

        while (tokenizer.HasMoreTokens()) {
            parts.push_back(tokenizer.GetNextToken());
        }
        if (parts.empty()) {
            return;
        }

        if (parts[0] == "SCAN_PROGRESS" && parts.size() >= 5) {
            long index = 0;
            long total = 0;

            parts[1].ToLong(&index);
            parts[2].ToLong(&total);
            progress_->SetRange(std::max(1L, total));
            progress_->SetValue(static_cast<int>(index));
            channelLabel_->SetLabel(wxString::Format("Channel: %s", parts[3]));
            statusLabel_->SetLabel(
                wxString::Format("Scanning %s (%ld of %ld)", parts[3], index, total)
            );
            return;
        }

        if (parts[0] == "SCAN_LOCK" && parts.size() >= 7) {
            long services = 0;
            long labels = 0;

            parts[5].ToLong(&services);
            parts[6].ToLong(&labels);
            channelLabel_->SetLabel(wxString::Format("Channel: %s", parts[3]));
            statusLabel_->SetLabel(
                wxString::Format("Locked on %s, services=%ld, labels=%ld", parts[3], services, labels)
            );
            return;
        }

        if (parts[0] == "SCAN_COMPLETE" && parts.size() >= 3) {
            long services = 0;
            parts[2].ToLong(&services);
            progress_->SetValue(progress_->GetRange());
            statusLabel_->SetLabel(wxString::Format("Scan complete. Services found: %ld", services));
            return;
        }

        if (parts[0] == "SCAN_ERROR" && parts.size() >= 2) {
            statusLabel_->SetLabel(parts[1]);
        }
    }

    wxStaticText *statusLabel_ = nullptr;
    wxStaticText *channelLabel_ = nullptr;
    wxGauge *progress_ = nullptr;
    wxButton *closeButton_ = nullptr;
    wxString resultsPath_;
    std::string resultsPathUtf8_;
    wxTimer pollTimer_;
    std::thread workerThread_;
    std::mutex linesMutex_;
    std::deque<wxString> pendingLines_;
    std::atomic<bool> scanFinished_{false};
    std::atomic<int> scanResultCode_{-1};
    bool scanSucceeded_ = false;
};

}  // namespace

MainFrame::MainFrame()
    : wxFrame(nullptr, wxID_ANY, "dab-lib GUI", wxDefaultPosition, wxSize(1100, 760)) {
    SetMinClientSize(wxSize(900, 620));
    SetBackgroundColour(wxSystemSettings::GetColour(wxSYS_COLOUR_APPWORKSPACE));

    {
        wxFileName exePath(wxStandardPaths::Get().GetExecutablePath());
        wxFileName resultsPath(exePath.GetPath(), "dab_scan_results.tsv");
        scanResultsPath_ = resultsPath.GetFullPath();
    }

    auto *root = new wxPanel(this);
    auto *layout = new wxBoxSizer(wxVERTICAL);

    layout->Add(new SpectrumPanel(root), 3, wxEXPAND | wxALL, 10);
    layout->Add(new EqualizerPanel(root), 4, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 10);
    layout->Add(CreateBottomPanel(root), 2, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 10);

    root->SetSizer(layout);
    Centre();
    ReloadStationsFromScanResults();
    UpdatePlaybackControls();
}

wxWindow *MainFrame::CreateBottomPanel(wxWindow *parent) {
    auto *panel = new wxPanel(parent);
    panel->SetBackgroundColour(wxColour(223, 228, 234));

    auto *layout = new wxBoxSizer(wxHORIZONTAL);
    layout->Add(new StationLogoPanel(panel), 0, wxEXPAND | wxALL, 12);

    auto *controls = new wxBoxSizer(wxVERTICAL);
    auto *title = new wxStaticText(panel, wxID_ANY, "Playback controls");
    wxFont titleFont = title->GetFont();
    titleFont.SetWeight(wxFONTWEIGHT_BOLD);
    title->SetFont(titleFont);

    controlStatusLabel_ = new wxStaticText(panel, wxID_ANY, "Scan stations to populate the list");
    controlStatusLabel_->SetForegroundColour(wxColour(95, 104, 116));

    auto *stationLabel = new wxStaticText(panel, wxID_ANY, "Station");
    stationChoice_ = new wxChoice(panel, wxID_ANY);

    auto *buttonRow = new wxBoxSizer(wxHORIZONTAL);
    auto *scanButton = new wxButton(panel, wxID_ANY, "Scan");
    startButton_ = new wxButton(panel, wxID_ANY, "Start");
    stopButton_ = new wxButton(panel, wxID_ANY, "Stop");
    scanButton->SetMinSize(wxSize(120, 38));
    startButton_->SetMinSize(wxSize(120, 38));
    stopButton_->SetMinSize(wxSize(120, 38));

    buttonRow->Add(scanButton, 0, wxRIGHT, 10);
    buttonRow->Add(startButton_, 0, wxRIGHT, 10);
    buttonRow->Add(stopButton_, 0);

    scanButton->Bind(wxEVT_BUTTON, &MainFrame::OnScanButton, this);
    startButton_->Bind(wxEVT_BUTTON, &MainFrame::OnStartButton, this);
    stopButton_->Bind(wxEVT_BUTTON, &MainFrame::OnStopButton, this);
    stationChoice_->Bind(wxEVT_CHOICE, &MainFrame::OnStationSelected, this);

    controls->Add(title, 0, wxBOTTOM, 4);
    controls->Add(controlStatusLabel_, 0, wxBOTTOM, 14);
    controls->Add(stationLabel, 0, wxBOTTOM, 6);
    controls->Add(stationChoice_, 0, wxEXPAND | wxBOTTOM, 12);
    controls->Add(buttonRow, 0);

    layout->Add(controls, 1, wxEXPAND | wxTOP | wxRIGHT | wxBOTTOM, 18);
    panel->SetSizer(layout);
    return panel;
}

void MainFrame::OnScanButton(wxCommandEvent &) {
    ScanDialog dialog(this, scanResultsPath_);

    if (dialog.ShowModal() == wxID_OK && dialog.ScanSucceeded()) {
        ReloadStationsFromScanResults();
    }
}

void MainFrame::OnStartButton(wxCommandEvent &) {
    StartPlaybackForCurrentSelection();
}

void MainFrame::StartPlaybackForCurrentSelection() {
    long selection = wxNOT_FOUND;
    unsigned long sid = 0;

    if (stationChoice_ == nullptr) {
        return;
    }
    if (dab_gui_playback_is_running() != 0) {
        if (controlStatusLabel_ != nullptr) {
            controlStatusLabel_->SetLabel("Playback is already running.");
        }
        UpdatePlaybackControls();
        return;
    }
    selection = stationChoice_->GetSelection();
    if (selection == wxNOT_FOUND || static_cast<size_t>(selection) >= stationEntries_.size()) {
        if (controlStatusLabel_ != nullptr) {
            controlStatusLabel_->SetLabel("Select a station first.");
        }
        UpdatePlaybackControls();
        return;
    }
    if (!stationEntries_[selection].serviceId.ToULong(&sid, 0)) {
        if (controlStatusLabel_ != nullptr) {
            controlStatusLabel_->SetLabel("Selected station has an invalid service ID.");
        }
        UpdatePlaybackControls();
        return;
    }

    if (dab_gui_playback_start(stationEntries_[selection].frequencyHz, (uint32_t)sid) != 0) {
        if (controlStatusLabel_ != nullptr) {
            controlStatusLabel_->SetLabel("Failed to start playback.");
        }
    } else if (controlStatusLabel_ != nullptr) {
        controlStatusLabel_->SetLabel(
            wxString::Format("Playing %s on %s", stationEntries_[selection].label, stationEntries_[selection].channel)
        );
    }
    UpdatePlaybackControls();
}

void MainFrame::OnStopButton(wxCommandEvent &) {
    StopPlayback();
}

void MainFrame::OnStationSelected(wxCommandEvent &) {
    if (stationChoice_ == nullptr) {
        return;
    }

    if (dab_gui_playback_is_running() != 0) {
        if (controlStatusLabel_ != nullptr) {
            controlStatusLabel_->SetLabel("Switching to selected station...");
        }
        StopPlayback();
        StartPlaybackForCurrentSelection();
    } else if (controlStatusLabel_ != nullptr && stationChoice_->GetSelection() != wxNOT_FOUND) {
        controlStatusLabel_->SetLabel("Station selected. Press Start to play.");
    }
    UpdatePlaybackControls();
}

void MainFrame::ReloadStationsFromScanResults() {
    FILE *fp = fopen(scanResultsPath_.utf8_string().c_str(), "rb");
    char line[512];

    if (stationChoice_ == nullptr || fp == nullptr) {
        return;
    }

    stationEntries_.clear();
    stationChoice_->Clear();

    if (fgets(line, sizeof(line), fp) == nullptr) {
        fclose(fp);
        return;
    }

    while (fgets(line, sizeof(line), fp) != nullptr) {
        wxStringTokenizer tokenizer(wxString::FromUTF8(line), "\t", wxTOKEN_RET_EMPTY_ALL);
        std::vector<wxString> fields;
        unsigned long long frequency_hz = 0u;
        unsigned long sid = 0u;

        while (tokenizer.HasMoreTokens()) {
            fields.push_back(tokenizer.GetNextToken());
        }
        if (fields.size() < 4) {
            continue;
        }
        fields[3].Trim(true);
        fields[3].Trim(false);
        if (!fields[1].ToULongLong(&frequency_hz) || !fields[2].ToULong(&sid, 0)) {
            continue;
        }

        {
            wxString label = fields[3];
            if (label.IsEmpty()) {
                label = wxString::Format("0x%08lx", sid);
            }

            stationEntries_.push_back({
                fields[0],
                label,
                wxString::Format("0x%08lx", sid),
                frequency_hz,
            });
            stationChoice_->Append(label);
        }
    }

    fclose(fp);

    if (!stationEntries_.empty()) {
        stationChoice_->SetSelection(0);
        if (controlStatusLabel_ != nullptr && dab_gui_playback_is_running() == 0) {
            controlStatusLabel_->SetLabel(
                wxString::Format("Loaded %zu station(s). Select one and press Start.", stationEntries_.size())
            );
        }
    } else if (controlStatusLabel_ != nullptr && dab_gui_playback_is_running() == 0) {
        controlStatusLabel_->SetLabel("Scan finished but no stations were found.");
    }

    UpdatePlaybackControls();
}

void MainFrame::UpdatePlaybackControls() {
    const bool hasStations = !stationEntries_.empty() && stationChoice_ != nullptr;
    const bool hasSelection = hasStations && stationChoice_->GetSelection() != wxNOT_FOUND;
    const bool isPlaying = dab_gui_playback_is_running() != 0;

    if (startButton_ != nullptr) {
        startButton_->Enable(hasSelection && !isPlaying);
    }
    if (stopButton_ != nullptr) {
        stopButton_->Enable(isPlaying);
    }
}

void MainFrame::StopPlayback() {
    if (dab_gui_playback_is_running() == 0) {
        if (controlStatusLabel_ != nullptr) {
            controlStatusLabel_->SetLabel("Playback is not running.");
        }
        UpdatePlaybackControls();
        return;
    }

    if (controlStatusLabel_ != nullptr) {
        controlStatusLabel_->SetLabel("Stopping playback...");
    }
    dab_gui_playback_stop();
    if (controlStatusLabel_ != nullptr) {
        controlStatusLabel_->SetLabel("Playback stopped.");
    }
    UpdatePlaybackControls();
}
