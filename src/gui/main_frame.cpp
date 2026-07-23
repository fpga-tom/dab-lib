#include "main_frame.hpp"
#include "dab_gui_backend.h"

#include "dab/core/audio_eq.h"

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
#include <wx/checkbox.h>
#include <wx/choice.h>
#include <wx/config.h>
#include <wx/dialog.h>
#include <wx/dcbuffer.h>
#include <wx/dcgraph.h>
#include <wx/filename.h>
#include <wx/gauge.h>
#include <wx/glcanvas.h>
#include <wx/image.h>
#include <wx/log.h>
#include <wx/menu.h>
#include <wx/mstream.h>
#include <wx/panel.h>
#include <wx/radiobox.h>
#include <wx/settings.h>
#include <wx/sizer.h>
#include <wx/slider.h>
#include <wx/spinctrl.h>
#include <wx/stdpaths.h>
#include <wx/stattext.h>
#include <wx/timer.h>
#include <wx/tokenzr.h>
#include <wx/utils.h>

#if defined(__APPLE__)
#include <OpenGL/gl.h>
#else
#include <GL/gl.h>
#endif

#if defined(_WIN32)
#include <windows.h>
#endif

namespace {

constexpr int kEqualizerTimerId = wxID_HIGHEST + 1;
constexpr int kScanTimerId = wxID_HIGHEST + 2;
constexpr int kPlaybackStatusTimerId = wxID_HIGHEST + 3;
constexpr int kSdrSettingsMenuId = wxID_HIGHEST + 4;
constexpr int kSpectrumSettingsMenuId = wxID_HIGHEST + 5;
constexpr int kAudioSettingsMenuId = wxID_HIGHEST + 6;

enum class SpectrumDisplayMode {
    RtaBars = 0,
    FftCurve = 1,
    LedBars = 2
};

static SpectrumDisplayMode gSpectrumDisplayMode = SpectrumDisplayMode::RtaBars;

static bool EnvEnabled(const char *name, bool fallback = false) {
    const char *value = std::getenv(name);

    if (value == nullptr || value[0] == '\0') {
        return fallback;
    }
    return std::strcmp(value, "0") != 0 &&
           std::strcmp(value, "false") != 0 &&
           std::strcmp(value, "False") != 0 &&
           std::strcmp(value, "off") != 0 &&
           std::strcmp(value, "OFF") != 0;
}

static wxBitmapType DetectBitmapType(const uint8_t *bytes, size_t size) {
    static const uint8_t kPngSignature[] = {0x89u, 'P', 'N', 'G', '\r', '\n', 0x1au, '\n'};
    static const uint8_t kJpegSignature[] = {0xffu, 0xd8u, 0xffu};
    static const uint8_t kGif87aSignature[] = {'G', 'I', 'F', '8', '7', 'a'};
    static const uint8_t kGif89aSignature[] = {'G', 'I', 'F', '8', '9', 'a'};
    static const uint8_t kBmpSignature[] = {'B', 'M'};

    if (bytes == nullptr || size == 0u) {
        return wxBITMAP_TYPE_INVALID;
    }
    if (size >= sizeof(kPngSignature) && memcmp(bytes, kPngSignature, sizeof(kPngSignature)) == 0) {
        return wxBITMAP_TYPE_PNG;
    }
    if (size >= sizeof(kJpegSignature) && memcmp(bytes, kJpegSignature, sizeof(kJpegSignature)) == 0) {
        return wxBITMAP_TYPE_JPEG;
    }
    if ((size >= sizeof(kGif87aSignature) && memcmp(bytes, kGif87aSignature, sizeof(kGif87aSignature)) == 0) ||
        (size >= sizeof(kGif89aSignature) && memcmp(bytes, kGif89aSignature, sizeof(kGif89aSignature)) == 0)) {
        return wxBITMAP_TYPE_GIF;
    }
    if (size >= sizeof(kBmpSignature) && memcmp(bytes, kBmpSignature, sizeof(kBmpSignature)) == 0) {
        return wxBITMAP_TYPE_BMP;
    }
    return wxBITMAP_TYPE_ANY;
}

class SpectrumPanel : public wxGLCanvas {
public:
    explicit SpectrumPanel(wxWindow *parent)
        : wxGLCanvas(parent, wxID_ANY, kGlAttributes, wxDefaultPosition, wxDefaultSize, wxFULL_REPAINT_ON_RESIZE),
          glContext_(new wxGLContext(this)),
          timer_(this, kEqualizerTimerId) {
        SetMinSize(wxSize(-1, 190));
        levels_.fill(0.0f);
        peaks_.fill(0.0f);

        Bind(wxEVT_PAINT, &SpectrumPanel::OnPaint, this);
        Bind(wxEVT_TIMER, &SpectrumPanel::OnTimer, this, kEqualizerTimerId);
        Bind(wxEVT_SIZE, &SpectrumPanel::OnSize, this);
        Bind(wxEVT_ERASE_BACKGROUND, &SpectrumPanel::OnEraseBackground, this);
        timer_.Start(40);
    }

    ~SpectrumPanel() override {
        SetCurrent(*glContext_);
        DestroyGlFont();
        delete glContext_;
        glContext_ = nullptr;
    }

private:
    static constexpr int kGlAttributes[] = {
        WX_GL_RGBA,
        WX_GL_DOUBLEBUFFER,
        WX_GL_DEPTH_SIZE, 16,
        0
    };

    static float BinToFrequencyHz(size_t index, size_t count) {
        static constexpr float kMinHz = 20.0f;
        static constexpr float kMaxHz = 20000.0f;
        const float t = count > 1u ? static_cast<float>(index) / static_cast<float>(count - 1u) : 0.0f;
        return kMinHz * std::pow(kMaxHz / kMinHz, t);
    }

    static wxString FormatFrequencyLabel(float frequencyHz) {
        if (frequencyHz >= 1000.0f) {
            const float khz = frequencyHz / 1000.0f;
            if (khz >= 10.0f || std::fabs(khz - std::round(khz)) < 0.05f) {
                return wxString::Format("%.0fk", khz);
            }
            return wxString::Format("%.1fk", khz);
        }
        return wxString::Format("%.0f", frequencyHz);
    }

    int FrequencyToX(const wxRect &plot, float frequencyHz) const {
        static constexpr float kMinHz = 20.0f;
        static constexpr float kMaxHz = 20000.0f;
        const double logMin = std::log10(static_cast<double>(kMinHz));
        const double logMax = std::log10(static_cast<double>(kMaxHz));
        const double logValue = std::log10(std::clamp(static_cast<double>(frequencyHz),
                                                      static_cast<double>(kMinHz),
                                                      static_cast<double>(kMaxHz)));
        const double t = (logValue - logMin) / (logMax - logMin);
        return plot.x + static_cast<int>(std::lround(t * static_cast<double>(plot.width)));
    }

    int LevelToY(const wxRect &plot, float level) const {
        const float clamped = std::clamp(level, 0.0f, 1.0f);
        return plot.GetBottom() - static_cast<int>(std::lround(clamped * static_cast<float>(plot.height)));
    }

    float VisualLevel(float level) const {
        const float clamped = std::clamp(level, 0.0f, 1.0f);
        return std::pow(clamped, 0.68f);
    }

    static void SetGlColour(const wxColour &colour) {
        glColor4ub(colour.Red(), colour.Green(), colour.Blue(), colour.Alpha());
    }

    static void DrawGlQuad(int x, int y, int width, int height, const wxColour &base, const wxColour &top) {
        glBegin(GL_QUADS);
        SetGlColour(base);
        glVertex2i(x, y + height);
        glVertex2i(x + width, y + height);
        SetGlColour(top);
        glVertex2i(x + width, y);
        glVertex2i(x, y);
        glEnd();
    }

    static void DrawGlLine(int x0, int y0, int x1, int y1, const wxColour &colour, float width = 1.0f) {
        glLineWidth(width);
        glBegin(GL_LINES);
        SetGlColour(colour);
        glVertex2i(x0, y0);
        glVertex2i(x1, y1);
        glEnd();
    }

    static void DrawGlRectOutline(const wxRect &rect, const wxColour &colour) {
        glLineWidth(1.0f);
        glBegin(GL_LINE_LOOP);
        SetGlColour(colour);
        glVertex2i(rect.x, rect.y);
        glVertex2i(rect.GetRight(), rect.y);
        glVertex2i(rect.GetRight(), rect.GetBottom());
        glVertex2i(rect.x, rect.GetBottom());
        glEnd();
    }

    void EnsureGlFont() {
#if defined(_WIN32)
        if (glFontReady_) {
            return;
        }

        HDC dc = wglGetCurrentDC();
        if (dc == 0) {
            return;
        }

        glFontBase_ = glGenLists(96);
        if (glFontBase_ == 0) {
            return;
        }

        HFONT font = CreateFontA(-13,
                                 0,
                                 0,
                                 0,
                                 FW_SEMIBOLD,
                                 FALSE,
                                 FALSE,
                                 FALSE,
                                 ANSI_CHARSET,
                                 OUT_TT_PRECIS,
                                 CLIP_DEFAULT_PRECIS,
                                 ANTIALIASED_QUALITY,
                                 FF_DONTCARE | DEFAULT_PITCH,
                                 "Segoe UI");
        if (font == 0) {
            glDeleteLists(glFontBase_, 96);
            glFontBase_ = 0;
            return;
        }

        HGDIOBJ old = SelectObject(dc, font);
        if (!wglUseFontBitmapsA(dc, 32, 96, glFontBase_)) {
            SelectObject(dc, old);
            DeleteObject(font);
            glDeleteLists(glFontBase_, 96);
            glFontBase_ = 0;
            return;
        }
        SelectObject(dc, old);
        DeleteObject(font);
        glFontReady_ = true;
#endif
    }

    void DestroyGlFont() {
#if defined(_WIN32)
        if (glFontBase_ != 0) {
            glDeleteLists(glFontBase_, 96);
            glFontBase_ = 0;
        }
        glFontReady_ = false;
#endif
    }

    void DrawGlText(int x, int y, const wxColour &colour, const std::string &text) {
#if defined(_WIN32)
        EnsureGlFont();
        if (!glFontReady_ || text.empty()) {
            return;
        }
        SetGlColour(colour);
        glRasterPos2i(x, y);
        glListBase(glFontBase_ - 32);
        glCallLists(static_cast<GLsizei>(text.size()), GL_UNSIGNED_BYTE, text.c_str());
#else
        (void)x;
        (void)y;
        (void)colour;
        (void)text;
#endif
    }

    void RenderOpenGL(const wxRect &frame, const wxRect &plot) {
        const wxRect rect = GetClientRect();
        const double scale = std::max(1.0, static_cast<double>(GetContentScaleFactor()));
        const int viewportWidth = std::max(1, static_cast<int>(std::lround(static_cast<double>(rect.width) * scale)));
        const int viewportHeight = std::max(1, static_cast<int>(std::lround(static_cast<double>(rect.height) * scale)));
        const std::array<int, 7> dbLabels = {0, -12, -24, -36, -48, -60, -70};
        const std::array<float, 10> freqMarkers = {20.0f, 50.0f, 100.0f, 200.0f, 500.0f,
                                                   1000.0f, 2000.0f, 5000.0f, 10000.0f, 20000.0f};
        size_t i;
        const size_t visibleBins = levels_.size();
        std::array<int, DAB_AUDIO_SPECTRUM_DISPLAY_BINS> traceX{};
        std::array<int, DAB_AUDIO_SPECTRUM_DISPLAY_BINS> traceY{};
        std::array<int, DAB_AUDIO_SPECTRUM_DISPLAY_BINS> peakY{};

        SetCurrent(*glContext_);
        glViewport(0, 0, viewportWidth, viewportHeight);
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_CULL_FACE);
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glClearColor(0.035f, 0.055f, 0.078f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);

        glMatrixMode(GL_PROJECTION);
        glLoadIdentity();
        glOrtho(0.0, static_cast<double>(rect.width), static_cast<double>(rect.height), 0.0, -1.0, 1.0);
        glMatrixMode(GL_MODELVIEW);
        glLoadIdentity();

        DrawGlQuad(rect.x, rect.y, rect.width, rect.height, wxColour(8, 12, 18, 255), wxColour(10, 16, 22, 255));
        DrawGlQuad(frame.x, frame.y, frame.width, frame.height, wxColour(12, 18, 25, 255), wxColour(14, 21, 29, 255));
        DrawGlRectOutline(frame, wxColour(35, 47, 58, 255));
        DrawGlQuad(plot.x, plot.y, plot.width, plot.height, wxColour(10, 16, 22, 255), wxColour(14, 22, 30, 255));

        for (size_t row = 0; row < dbLabels.size(); ++row) {
            const float t = static_cast<float>(-dbLabels[row]) / 70.0f;
            const int y = plot.y + static_cast<int>(std::lround(t * static_cast<float>(plot.height)));
            const wxColour gridColour = row == 0u ? wxColour(66, 82, 96, 255) : wxColour(30, 41, 52, 255);

            DrawGlLine(plot.x, y, plot.GetRight(), y, gridColour);
            DrawGlText(frame.x + 6, y + 4, wxColour(122, 138, 152), std::to_string(dbLabels[row]));
        }

        for (float marker : freqMarkers) {
            const int x = FrequencyToX(plot, marker);
            DrawGlLine(x, plot.y, x, plot.GetBottom(), wxColour(24, 35, 45));
            DrawGlText(x - 12, plot.GetBottom() + 18, wxColour(98, 116, 132), FormatFrequencyLabel(marker).ToStdString());
        }

        DrawGlText(frame.x + 10, frame.y + 16, wxColour(210, 221, 230), "Spectrum Analyzer");
        const char *displayLabel = "FFT 1024  |  LOG FREQ  |  dBFS";
        if (gSpectrumDisplayMode == SpectrumDisplayMode::RtaBars) {
            displayLabel = "RTA BARS  |  LOG FREQ  |  dBFS";
        } else if (gSpectrumDisplayMode == SpectrumDisplayMode::LedBars) {
            displayLabel = "LED BARS  |  LOG FREQ  |  dBFS";
        }
        DrawGlText(frame.GetRight() - 205, frame.y + 16, wxColour(120, 146, 163), displayLabel);

        for (i = 0; i < visibleBins; ++i) {
            static constexpr float kMinHz = 20.0f;
            static constexpr float kMaxHz = 20000.0f;
            const float t = (static_cast<float>(i) + 0.5f) / static_cast<float>(visibleBins);
            const float centerHz = kMinHz * std::pow(kMaxHz / kMinHz, t);
            const float level = std::clamp(VisualLevel(levels_[i]), 0.0f, 1.0f);
            const float peak = std::clamp(std::max(level, VisualLevel(peaks_[i])), 0.0f, 1.0f);

            traceX[i] = FrequencyToX(plot, centerHz);
            traceY[i] = LevelToY(plot, level);
            peakY[i] = LevelToY(plot, peak);
        }

        if (gSpectrumDisplayMode == SpectrumDisplayMode::RtaBars) {
            static constexpr float kMinHz = 20.0f;
            static constexpr float kMaxHz = 20000.0f;

            for (i = 0; i < visibleBins; ++i) {
                const float startT = static_cast<float>(i) / static_cast<float>(visibleBins);
                const float endT = static_cast<float>(i + 1u) / static_cast<float>(visibleBins);
                const float startHz = kMinHz * std::pow(kMaxHz / kMinHz, startT);
                const float endHz = kMinHz * std::pow(kMaxHz / kMinHz, endT);
                const int bandLeft = FrequencyToX(plot, startHz);
                const int bandRight = FrequencyToX(plot, endHz);
                const int gap = std::max(2, (bandRight - bandLeft) / 8);
                const int x = std::min(bandRight - 1, bandLeft + gap);
                const int width = std::max(3, bandRight - bandLeft - (2 * gap));
                const int y = traceY[i];
                const int height = std::max(1, plot.GetBottom() - y);

                DrawGlQuad(x,
                           y,
                           width,
                           height,
                           wxColour(28, 128, 156, 210),
                           wxColour(105, 225, 238, 238));
                DrawGlQuad(x,
                           y,
                           width,
                           std::max(2, height / 5),
                           wxColour(205, 252, 255, 36),
                           wxColour(235, 255, 255, 86));
                DrawGlLine(x, peakY[i], x + width, peakY[i], wxColour(255, 184, 92, 215), 2.0f);
            }
        } else if (gSpectrumDisplayMode == SpectrumDisplayMode::LedBars) {
            static constexpr float kMinHz = 20.0f;
            static constexpr float kMaxHz = 20000.0f;
            const int segmentGap = 2;
            const int segmentCount = std::clamp(plot.height / 9, 8, 28);
            const int segmentStride = std::max(4, plot.height / segmentCount);
            const int segmentHeight = std::max(2, segmentStride - segmentGap);

            for (i = 0; i < visibleBins; ++i) {
                const float startT = static_cast<float>(i) / static_cast<float>(visibleBins);
                const float endT = static_cast<float>(i + 1u) / static_cast<float>(visibleBins);
                const float startHz = kMinHz * std::pow(kMaxHz / kMinHz, startT);
                const float endHz = kMinHz * std::pow(kMaxHz / kMinHz, endT);
                const int bandLeft = FrequencyToX(plot, startHz);
                const int bandRight = FrequencyToX(plot, endHz);
                const int gap = std::max(2, (bandRight - bandLeft) / 8);
                const int x = std::min(bandRight - 1, bandLeft + gap);
                const int width = std::max(3, bandRight - bandLeft - (2 * gap));
                const float level = std::clamp(VisualLevel(levels_[i]) * 1.16f, 0.0f, 1.0f);
                const float peak = std::clamp(
                    std::max(level, VisualLevel(peaks_[i]) * 1.16f),
                    0.0f,
                    1.0f);
                const int litSegments = std::clamp(
                    static_cast<int>(std::ceil(level * static_cast<float>(segmentCount))),
                    0,
                    segmentCount);
                const int peakSegment = std::clamp(
                    static_cast<int>(std::ceil(peak * static_cast<float>(segmentCount))) - 1,
                    0,
                    segmentCount - 1);

                for (int segment = 0; segment < segmentCount; ++segment) {
                    const float position = (static_cast<float>(segment) + 0.5f) /
                                           static_cast<float>(segmentCount);
                    const bool lit = segment < litSegments;
                    const bool isPeak = segment == peakSegment && peak > 0.02f;
                    wxColour base;
                    wxColour top;
                    wxColour glow;

                    if (position >= 0.72f) {
                        base = lit ? wxColour(190, 30, 25, 255) : wxColour(48, 18, 20, 210);
                        top = lit ? wxColour(255, 92, 50, 255) : wxColour(72, 25, 24, 220);
                        glow = wxColour(255, 52, 28);
                    } else if (position >= 0.52f) {
                        base = lit ? wxColour(205, 126, 15, 255) : wxColour(49, 38, 15, 210);
                        top = lit ? wxColour(255, 220, 48, 255) : wxColour(70, 53, 19, 220);
                        glow = wxColour(255, 190, 30);
                    } else {
                        base = lit ? wxColour(18, 150, 70, 255) : wxColour(15, 43, 31, 210);
                        top = lit ? wxColour(78, 245, 116, 255) : wxColour(19, 62, 40, 220);
                        glow = wxColour(40, 255, 105);
                    }

                    const int y = plot.GetBottom() - ((segment + 1) * segmentStride);
                    if (lit) {
                        glBlendFunc(GL_SRC_ALPHA, GL_ONE);
                        DrawGlQuad(x - 4,
                                   y - 3,
                                   width + 8,
                                   segmentHeight + 6,
                                   wxColour(glow.Red(), glow.Green(), glow.Blue(), 14),
                                   wxColour(glow.Red(), glow.Green(), glow.Blue(), 22));
                        DrawGlQuad(x - 2,
                                   y - 2,
                                   width + 4,
                                   segmentHeight + 4,
                                   wxColour(glow.Red(), glow.Green(), glow.Blue(), 30),
                                   wxColour(glow.Red(), glow.Green(), glow.Blue(), 48));
                        DrawGlQuad(x - 1,
                                   y - 1,
                                   width + 2,
                                   segmentHeight + 2,
                                   wxColour(glow.Red(), glow.Green(), glow.Blue(), 55),
                                   wxColour(glow.Red(), glow.Green(), glow.Blue(), 78));
                        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
                    }
                    DrawGlQuad(x, y, width, segmentHeight, base, top);
                    if (lit) {
                        DrawGlLine(x + 1,
                                   y + 1,
                                   x + width - 1,
                                   y + 1,
                                   wxColour(245, 255, 247, 185));
                    }
                    if (isPeak) {
                        DrawGlRectOutline(wxRect(x, y, width, segmentHeight), wxColour(255, 255, 225, 245));
                    }
                }
            }
        } else {
            glBegin(GL_TRIANGLE_STRIP);
            for (i = 0; i < visibleBins; ++i) {
                SetGlColour(wxColour(46, 196, 220, 96));
                glVertex2i(traceX[i], traceY[i]);
                SetGlColour(wxColour(46, 196, 220, 8));
                glVertex2i(traceX[i], plot.GetBottom());
            }
            glEnd();

            glLineWidth(5.0f);
            glBegin(GL_LINE_STRIP);
            SetGlColour(wxColour(46, 196, 220, 38));
            for (i = 0; i < visibleBins; ++i) {
                glVertex2i(traceX[i], traceY[i]);
            }
            glEnd();

            glLineWidth(2.0f);
            glBegin(GL_LINE_STRIP);
            SetGlColour(wxColour(116, 232, 242, 235));
            for (i = 0; i < visibleBins; ++i) {
                glVertex2i(traceX[i], traceY[i]);
            }
            glEnd();

            glPointSize(3.0f);
            glBegin(GL_POINTS);
            SetGlColour(wxColour(180, 246, 250, 220));
            for (i = 0; i < visibleBins; ++i) {
                glVertex2i(traceX[i], traceY[i]);
            }
            glEnd();

            glLineWidth(1.5f);
            glBegin(GL_LINE_STRIP);
            SetGlColour(wxColour(255, 184, 92, 190));
            for (i = 0; i < visibleBins; ++i) {
                glVertex2i(traceX[i], peakY[i]);
            }
            glEnd();
        }

        glFlush();
        SwapBuffers();
    }

    void OnTimer(wxTimerEvent &) {
        dab_gui_playback_spectrum_t spectrum{};

        if (dab_gui_playback_get_spectrum(&spectrum) == 0 && spectrum.valid && spectrum.bin_count > 0u) {
            const size_t count = std::min(levels_.size(), static_cast<size_t>(spectrum.bin_count));

            for (size_t i = 0; i < count; ++i) {
                levels_[i] = std::clamp(spectrum.bins[i], 0.0f, 1.0f);
                peaks_[i] = std::max(levels_[i], peaks_[i] * 0.91f);
            }
            for (size_t i = count; i < levels_.size(); ++i) {
                levels_[i] *= 0.90f;
                peaks_[i] *= 0.86f;
            }
        } else {
            for (float &level : levels_) {
                level *= 0.88f;
            }
            for (float &peak : peaks_) {
                peak *= 0.88f;
            }
        }
        Refresh(false);
    }

    void OnPaint(wxPaintEvent &) {
        wxPaintDC dc(this);

        const wxRect rect = GetClientRect();
        const wxRect frame(rect.x + 2, rect.y + 2, std::max(1, rect.width - 4), std::max(1, rect.height - 4));
        const wxRect plot(frame.x + 18, frame.y + 18, std::max(1, frame.width - 30), std::max(1, frame.height - 32));

        RenderOpenGL(frame, plot);
    }

    void OnSize(wxSizeEvent &event) {
        Refresh(false);
        event.Skip();
    }

    void OnEraseBackground(wxEraseEvent &) {
    }

    std::array<float, DAB_AUDIO_SPECTRUM_DISPLAY_BINS> levels_;
    std::array<float, DAB_AUDIO_SPECTRUM_DISPLAY_BINS> peaks_;
    wxGLContext *glContext_ = nullptr;
    wxTimer timer_;
#if defined(_WIN32)
    GLuint glFontBase_ = 0;
    bool glFontReady_ = false;
#endif
};

class StationLogoPanel : public wxPanel {
public:
    explicit StationLogoPanel(wxWindow *parent) : wxPanel(parent) {
        SetMinSize(wxSize(220, 110));
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        SetBackgroundColour(wxColour(24, 28, 34));
        Bind(wxEVT_PAINT, &StationLogoPanel::OnPaint, this);
    }

    void ClearLogo() {
        hasLogo_ = false;
        logoImage_ = wxImage();
        Refresh(false);
    }

    void SetLogoImage(const wxImage &image) {
        if (!image.IsOk()) {
            ClearLogo();
            return;
        }

        logoImage_ = image.Copy();
        hasLogo_ = true;
        Refresh(false);
    }

private:
    void OnPaint(wxPaintEvent &) {
        wxAutoBufferedPaintDC dc(this);
        const wxRect rect = GetClientRect();
        dc.SetBackground(wxBrush(GetBackgroundColour()));
        dc.Clear();

        if (hasLogo_ && logoImage_.IsOk()) {
            const double scaleX = static_cast<double>(std::max(1, rect.width)) /
                                  static_cast<double>(logoImage_.GetWidth());
            const double scaleY = static_cast<double>(std::max(1, rect.height)) /
                                  static_cast<double>(logoImage_.GetHeight());
            const double scale = std::min(scaleX, scaleY);
            const int drawWidth = std::max(1, static_cast<int>(std::lround(logoImage_.GetWidth() * scale)));
            const int drawHeight = std::max(1, static_cast<int>(std::lround(logoImage_.GetHeight() * scale)));
            const int drawX = rect.x + ((rect.width - drawWidth) / 2);
            const int drawY = rect.y + ((rect.height - drawHeight) / 2);
            wxBitmap bitmap(logoImage_.Scale(drawWidth, drawHeight, wxIMAGE_QUALITY_HIGH));

            dc.DrawBitmap(bitmap, drawX, drawY, true);
            return;
        }
    }

    wxImage logoImage_;
    bool hasLogo_ = false;
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
                    PushEqualizerToBackend();
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

    void PushEqualizerToBackend() {
        float gains[DAB_AUDIO_EQ_BAND_COUNT] = {0};

        for (size_t i = 0; i < sliders_.size() && i < DAB_AUDIO_EQ_BAND_COUNT; ++i) {
            gains[i] = (float)sliders_[i]->GetValue();
        }
        (void)dab_gui_playback_set_equalizer(gains, DAB_AUDIO_EQ_BAND_COUNT);
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
        PushEqualizerToBackend();
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

class SdrSettingsDialog : public wxDialog {
public:
    SdrSettingsDialog(wxWindow *parent, const dab_gui_sdr_settings_t &settings)
        : wxDialog(parent, wxID_ANY, "SDR Settings", wxDefaultPosition, wxDefaultSize,
                   wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER) {
        auto *outer = new wxBoxSizer(wxVERTICAL);
        wxString backendChoices[] = {"SDRplay", "RTL-SDR"};

        backendChoice_ = new wxRadioBox(this,
                                        wxID_ANY,
                                        "HAL backend",
                                        wxDefaultPosition,
                                        wxDefaultSize,
                                        2,
                                        backendChoices,
                                        1,
                                        wxRA_SPECIFY_COLS);
        backendChoice_->SetSelection(settings.backend == DAB_GUI_SDR_BACKEND_RTLSDR ? 1 : 0);

        agcCheck_ = new wxCheckBox(this, wxID_ANY, "Automatic gain control");
        agcCheck_->SetValue(settings.agc_enabled != 0);

        auto *gainRow = new wxBoxSizer(wxHORIZONTAL);
        gainLabel_ = new wxStaticText(this, wxID_ANY, "Manual gain");
        gainSpin_ = new wxSpinCtrl(this, wxID_ANY);
        gainSpin_->SetRange(0, 59);
        gainSpin_->SetValue(std::clamp(settings.gain_db, 0, 59));

        gainRow->Add(gainLabel_, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 10);
        gainRow->Add(gainSpin_, 0, wxALIGN_CENTER_VERTICAL);
        gainRow->Add(new wxStaticText(this, wxID_ANY, "dB"), 0, wxALIGN_CENTER_VERTICAL | wxLEFT, 6);

        auto *buttons = new wxStdDialogButtonSizer();
        buttons->AddButton(new wxButton(this, wxID_OK));
        buttons->AddButton(new wxButton(this, wxID_CANCEL));
        buttons->Realize();

        outer->Add(backendChoice_, 0, wxEXPAND | wxALL, 14);
        outer->Add(agcCheck_, 0, wxLEFT | wxRIGHT | wxBOTTOM, 14);
        outer->Add(gainRow, 0, wxLEFT | wxRIGHT | wxBOTTOM, 14);
        outer->Add(buttons, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 14);

        SetSizerAndFit(outer);
        SetMinSize(wxSize(360, -1));
        CentreOnParent();

        agcCheck_->Bind(wxEVT_CHECKBOX, [this](wxCommandEvent &) {
            UpdateGainControls();
        });
        backendChoice_->Bind(wxEVT_RADIOBOX, [this](wxCommandEvent &) {
            UpdateGainControls();
        });
        UpdateGainControls();
    }

    dab_gui_sdr_settings_t Settings() const {
        dab_gui_sdr_settings_t settings;
        settings.backend = backendChoice_->GetSelection() == 1 ?
            DAB_GUI_SDR_BACKEND_RTLSDR : DAB_GUI_SDR_BACKEND_SDRPLAY;
        settings.agc_enabled = agcCheck_->GetValue() ? 1 : 0;
        settings.gain_db = gainSpin_->GetValue();
        return settings;
    }

private:
    void UpdateGainControls() {
        const bool manual = !agcCheck_->GetValue();
        const int maxGain = backendChoice_->GetSelection() == 1 ? 49 : 59;
        if (gainSpin_->GetMax() != maxGain) {
            gainSpin_->SetRange(0, maxGain);
        }
        if (gainSpin_->GetValue() > maxGain) {
            gainSpin_->SetValue(maxGain);
        }
        gainLabel_->Enable(manual);
        gainSpin_->Enable(manual);
    }

    wxRadioBox *backendChoice_ = nullptr;
    wxCheckBox *agcCheck_ = nullptr;
    wxStaticText *gainLabel_ = nullptr;
    wxSpinCtrl *gainSpin_ = nullptr;
};

class AudioSettingsDialog : public wxDialog {
public:
    AudioSettingsDialog(wxWindow *parent, const dab_gui_audio_settings_t &settings)
        : wxDialog(parent, wxID_ANY, "Audio Settings", wxDefaultPosition, wxDefaultSize,
                   wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER) {
        auto *outer = new wxBoxSizer(wxVERTICAL);
        wxString backendChoices[] = {
            "Automatic (recommended)",
            "Windows Audio (WASAPI)",
            "PortAudio"
        };

        backendChoice_ = new wxRadioBox(this,
                                        wxID_ANY,
                                        "Audio backend",
                                        wxDefaultPosition,
                                        wxDefaultSize,
                                        3,
                                        backendChoices,
                                        1,
                                        wxRA_SPECIFY_COLS);
        if (settings.backend == DAB_AUDIO_OUTPUT_WASAPI) {
            backendChoice_->SetSelection(1);
        } else if (settings.backend == DAB_AUDIO_OUTPUT_PORTAUDIO) {
            backendChoice_->SetSelection(2);
        } else {
            backendChoice_->SetSelection(0);
        }
        backendChoice_->Enable(1, dab_audio_output_backend_available(DAB_AUDIO_OUTPUT_WASAPI) != 0);
        backendChoice_->Enable(2, dab_audio_output_backend_available(DAB_AUDIO_OUTPUT_PORTAUDIO) != 0);

        auto *description = new wxStaticText(
            this,
            wxID_ANY,
            "Automatic prefers native Windows Audio and falls back to PortAudio.");
        description->SetForegroundColour(wxColour(95, 104, 116));
        description->Wrap(390);

        auto *buttons = new wxStdDialogButtonSizer();
        buttons->AddButton(new wxButton(this, wxID_OK));
        buttons->AddButton(new wxButton(this, wxID_CANCEL));
        buttons->Realize();

        outer->Add(backendChoice_, 0, wxEXPAND | wxALL, 14);
        outer->Add(description, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 14);
        outer->Add(buttons, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 14);

        SetSizerAndFit(outer);
        SetMinSize(wxSize(430, -1));
        CentreOnParent();
    }

    dab_gui_audio_settings_t Settings() const {
        dab_gui_audio_settings_t settings;

        if (backendChoice_->GetSelection() == 1) {
            settings.backend = DAB_AUDIO_OUTPUT_WASAPI;
        } else if (backendChoice_->GetSelection() == 2) {
            settings.backend = DAB_AUDIO_OUTPUT_PORTAUDIO;
        } else {
            settings.backend = DAB_AUDIO_OUTPUT_AUTO;
        }
        return settings;
    }

private:
    wxRadioBox *backendChoice_ = nullptr;
};

class SpectrumSettingsDialog : public wxDialog {
public:
    SpectrumSettingsDialog(wxWindow *parent, SpectrumDisplayMode mode)
        : wxDialog(parent, wxID_ANY, "Spectrum Settings", wxDefaultPosition, wxDefaultSize,
                   wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER) {
        auto *outer = new wxBoxSizer(wxVERTICAL);
        wxString displayChoices[] = {"RTA bars", "FFT curve", "LED bars"};

        displayModeChoice_ = new wxRadioBox(this,
                                            wxID_ANY,
                                            "Display mode",
                                            wxDefaultPosition,
                                            wxDefaultSize,
                                            3,
                                            displayChoices,
                                            1,
                                            wxRA_SPECIFY_COLS);
        displayModeChoice_->SetSelection(static_cast<int>(mode));

        auto *buttons = new wxStdDialogButtonSizer();
        buttons->AddButton(new wxButton(this, wxID_OK));
        buttons->AddButton(new wxButton(this, wxID_CANCEL));
        buttons->Realize();

        outer->Add(displayModeChoice_, 0, wxEXPAND | wxALL, 14);
        outer->Add(buttons, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 14);

        SetSizerAndFit(outer);
        SetMinSize(wxSize(320, -1));
        CentreOnParent();
    }

    SpectrumDisplayMode DisplayMode() const {
        return static_cast<SpectrumDisplayMode>(displayModeChoice_->GetSelection());
    }

private:
    wxRadioBox *displayModeChoice_ = nullptr;
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

        closeButton_ = new wxButton(this, wxID_ANY, "Cancel");

        outer->Add(title, 0, wxALL, 16);
        outer->Add(statusLabel_, 0, wxLEFT | wxRIGHT | wxBOTTOM, 16);
        outer->Add(channelLabel_, 0, wxLEFT | wxRIGHT | wxBOTTOM, 12);
        outer->Add(progress_, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 16);
        outer->Add(closeButton_, 0, wxALIGN_RIGHT | wxLEFT | wxRIGHT | wxBOTTOM, 16);

        SetSizerAndFit(outer);
        SetMinSize(wxSize(420, 200));
        CentreOnParent();

        Bind(wxEVT_TIMER, &ScanDialog::OnPollTimer, this, kScanTimerId);
        Bind(wxEVT_CLOSE_WINDOW, &ScanDialog::OnClose, this);
        closeButton_->Bind(wxEVT_BUTTON, &ScanDialog::OnCloseButton, this);
        StartScan();
    }

    ~ScanDialog() override {
        StopScanWorker();
    }

    bool ScanSucceeded() const {
        return scanSucceeded_;
    }

private:
    static void OnScanEvent(const char *line, void *user) {
        auto *dialog = static_cast<ScanDialog *>(user);
        std::lock_guard<std::mutex> lock(dialog->linesMutex_);
        if (!dialog->callbacksEnabled_) {
            return;
        }
        dialog->pendingLines_.push_back(wxString::FromUTF8(line));
    }

    void StartScan() {
        dab_gui_sdr_settings_t settings;
        dab_gui_sdr_get_settings(&settings);
        statusLabel_->SetLabel(settings.backend == DAB_GUI_SDR_BACKEND_RTLSDR ?
            "Opening RTL-SDR backend..." : "Opening SDRplay backend...");
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
            FinalizeScan();
        }
    }

    void OnCloseButton(wxCommandEvent &) {
        CloseDialog();
    }

    void OnClose(wxCloseEvent &) {
        CloseDialog();
    }

    void CloseDialog() {
        const bool finished = scanFinished_.load();

        if (finished) {
            FinalizeScan();
        } else {
            if (statusLabel_ != nullptr) {
                statusLabel_->SetLabel("Cancelling scan...");
            }
            if (closeButton_ != nullptr) {
                closeButton_->Enable(false);
            }
            StopScanWorker();
        }
        EndModal(scanSucceeded_ ? wxID_OK : wxID_CANCEL);
    }

    void FinalizeScan() {
        if (scanFinalized_) {
            return;
        }
        pollTimer_.Stop();
        if (workerThread_.joinable()) {
            workerThread_.join();
        }
        {
            std::lock_guard<std::mutex> lock(linesMutex_);
            callbacksEnabled_ = false;
        }
        scanSucceeded_ = scanResultCode_.load() == 0;
        if (scanSucceeded_) {
            statusLabel_->SetLabel("Scan complete.");
            channelLabel_->SetLabel(wxString::Format("Results saved to %s", resultsPath_));
        } else if (scanResultCode_.load() == -2) {
            statusLabel_->SetLabel("Scan cancelled.");
        } else {
            statusLabel_->SetLabel("Scan failed before completion.");
        }
        closeButton_->SetLabel(scanSucceeded_ ? "Done" : "Close");
        closeButton_->Enable(true);
        scanFinalized_ = true;
    }

    void StopScanWorker() {
        pollTimer_.Stop();
        if (!scanFinished_.load()) {
            dab_gui_scan_cancel_current();
        }
        {
            std::lock_guard<std::mutex> lock(linesMutex_);
            callbacksEnabled_ = false;
            pendingLines_.clear();
        }
        if (workerThread_.joinable()) {
            workerThread_.join();
        }
        if (!scanFinished_.load()) {
            scanResultCode_.store(-2);
            scanFinished_.store(true);
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
    bool callbacksEnabled_ = true;
    std::atomic<bool> scanFinished_{false};
    std::atomic<int> scanResultCode_{-1};
    bool scanSucceeded_ = false;
    bool scanFinalized_ = false;
};

}  // namespace

MainFrame::MainFrame()
    : wxFrame(nullptr,
              wxID_ANY,
#if defined(DAB_GUI_APP_VERSION)
              "dab-lib GUI v" DAB_GUI_APP_VERSION,
#else
              "dab-lib GUI",
#endif
              wxDefaultPosition,
              wxSize(1100, 760)) {
    dab_gui_sdr_default_settings(&sdrSettings_);
    dab_gui_audio_default_settings(&audioSettings_);
    LoadSdrSettings();
    LoadAudioSettings();
    LoadSpectrumSettings();
    ApplySdrSettings();
    ApplyAudioSettings();

    auto *menuBar = new wxMenuBar();
    auto *settingsMenu = new wxMenu();
    settingsMenu->Append(kSdrSettingsMenuId, "SDR Settings...");
    settingsMenu->Append(kAudioSettingsMenuId, "Audio Settings...");
    settingsMenu->Append(kSpectrumSettingsMenuId, "Spectrum Settings...");
    menuBar->Append(settingsMenu, "Settings");
    SetMenuBar(menuBar);
    Bind(wxEVT_MENU, &MainFrame::OnSdrSettings, this, kSdrSettingsMenuId);
    Bind(wxEVT_MENU, &MainFrame::OnAudioSettings, this, kAudioSettingsMenuId);
    Bind(wxEVT_MENU, &MainFrame::OnSpectrumSettings, this, kSpectrumSettingsMenuId);

    SetMinClientSize(wxSize(900, 620));
    SetBackgroundColour(wxSystemSettings::GetColour(wxSYS_COLOUR_APPWORKSPACE));

    {
        wxFileName resultsPath;
#if defined(_WIN32)
        wxString userDataDir = wxStandardPaths::Get().GetUserLocalDataDir();
        if (!wxFileName::DirExists(userDataDir)) {
            (void)wxFileName::Mkdir(userDataDir, wxS_DIR_DEFAULT, wxPATH_MKDIR_FULL);
        }
        resultsPath.Assign(userDataDir, "dab_scan_results.tsv");
#else
        wxFileName exePath(wxStandardPaths::Get().GetExecutablePath());
        resultsPath.Assign(exePath.GetPath(), "dab_scan_results.tsv");
#endif
        scanResultsPath_ = resultsPath.GetFullPath();
    }

    auto *root = new wxPanel(this);
    auto *layout = new wxBoxSizer(wxVERTICAL);

    layout->Add(new SpectrumPanel(root), 3, wxEXPAND | wxALL, 10);
    layout->Add(new EqualizerPanel(root), 4, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 10);
    layout->Add(CreateBottomPanel(root), 2, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 10);

    root->SetSizer(layout);
    Centre();
    playbackStatusTimer_ = new wxTimer(this, kPlaybackStatusTimerId);
    Bind(wxEVT_TIMER, &MainFrame::OnPlaybackStatusTimer, this, kPlaybackStatusTimerId);
    playbackStatusTimer_->Start(400);
    ReloadStationsFromScanResults();
    UpdatePlaybackControls();
    RefreshPlaybackStatus();
}

void MainFrame::LoadSdrSettings() {
    wxConfig config("dab-lib");
    wxString backend;
    long gain = 40;
    bool agc = true;

    config.Read("/sdr/backend", &backend, "sdrplay");
    config.Read("/sdr/agc", &agc, true);
    config.Read("/sdr/gain_db", &gain, 40);

    sdrSettings_.backend = backend == "rtlsdr" ? DAB_GUI_SDR_BACKEND_RTLSDR : DAB_GUI_SDR_BACKEND_SDRPLAY;
    sdrSettings_.agc_enabled = agc ? 1 : 0;
    sdrSettings_.gain_db = std::clamp(static_cast<int>(gain), 0, 59);
    if (sdrSettings_.backend == DAB_GUI_SDR_BACKEND_RTLSDR && sdrSettings_.gain_db > 49) {
        sdrSettings_.gain_db = 49;
    }
}

void MainFrame::SaveSdrSettings() {
    wxConfig config("dab-lib");
    config.Write("/sdr/backend", sdrSettings_.backend == DAB_GUI_SDR_BACKEND_RTLSDR ? "rtlsdr" : "sdrplay");
    config.Write("/sdr/agc", sdrSettings_.agc_enabled != 0);
    config.Write("/sdr/gain_db", static_cast<long>(sdrSettings_.gain_db));
    config.Flush();
}

void MainFrame::LoadAudioSettings() {
    wxConfig config("dab-lib");
    wxString backend;

    config.Read("/audio/backend", &backend, "auto");
    audioSettings_.backend = dab_audio_output_backend_from_string(backend.utf8_string().c_str());
    if (audioSettings_.backend != DAB_AUDIO_OUTPUT_AUTO &&
        dab_audio_output_backend_available(audioSettings_.backend) == 0) {
        audioSettings_.backend = DAB_AUDIO_OUTPUT_AUTO;
    }
}

void MainFrame::SaveAudioSettings() {
    wxConfig config("dab-lib");

    config.Write("/audio/backend",
                 wxString::FromUTF8(dab_audio_output_backend_key(audioSettings_.backend)));
    config.Flush();
}

void MainFrame::LoadSpectrumSettings() {
    wxConfig config("dab-lib");
    wxString displayMode;

    config.Read("/spectrum/display_mode", &displayMode, "rta_bars");
    if (displayMode == "fft_curve") {
        gSpectrumDisplayMode = SpectrumDisplayMode::FftCurve;
    } else if (displayMode == "led_bars") {
        gSpectrumDisplayMode = SpectrumDisplayMode::LedBars;
    } else {
        gSpectrumDisplayMode = SpectrumDisplayMode::RtaBars;
    }
    spectrumDisplayMode_ = static_cast<int>(gSpectrumDisplayMode);
}

void MainFrame::SaveSpectrumSettings() {
    wxConfig config("dab-lib");
    wxString displayMode = "rta_bars";
    if (static_cast<SpectrumDisplayMode>(spectrumDisplayMode_) == SpectrumDisplayMode::FftCurve) {
        displayMode = "fft_curve";
    } else if (static_cast<SpectrumDisplayMode>(spectrumDisplayMode_) == SpectrumDisplayMode::LedBars) {
        displayMode = "led_bars";
    }
    config.Write("/spectrum/display_mode", displayMode);
    config.Flush();
}

void MainFrame::ApplySdrSettings() {
    if (sdrSettings_.backend == DAB_GUI_SDR_BACKEND_RTLSDR && sdrSettings_.gain_db > 49) {
        sdrSettings_.gain_db = 49;
    }
    dab_gui_sdr_set_settings(&sdrSettings_);
}

void MainFrame::ApplyAudioSettings() {
    dab_gui_audio_set_settings(&audioSettings_);
}

wxString MainFrame::SdrSettingsSummary() const {
    const wxString backend = sdrSettings_.backend == DAB_GUI_SDR_BACKEND_RTLSDR ? "RTL-SDR" : "SDRplay";
    if (sdrSettings_.agc_enabled != 0) {
        return wxString::Format("%s, AGC on", backend);
    }
    return wxString::Format("%s, AGC off, gain %d dB", backend, sdrSettings_.gain_db);
}

wxString MainFrame::AudioSettingsSummary() const {
    return wxString::FromUTF8(dab_audio_output_backend_name(audioSettings_.backend));
}

wxWindow *MainFrame::CreateBottomPanel(wxWindow *parent) {
    const bool showReceiverSection = EnvEnabled("DAB_GUI_SHOW_RECEIVER", false);
    auto *panel = new wxPanel(parent);
    panel->SetBackgroundColour(wxColour(223, 228, 234));

    auto *layout = new wxBoxSizer(wxHORIZONTAL);
    stationLogoPanel_ = new StationLogoPanel(panel);
    layout->Add(stationLogoPanel_, 0, wxEXPAND | wxALL, 12);

    auto *controls = new wxBoxSizer(wxVERTICAL);
    auto *content = new wxBoxSizer(wxHORIZONTAL);
    auto *title = new wxStaticText(panel, wxID_ANY, "Playback controls");
    wxFont titleFont = title->GetFont();
    titleFont.SetWeight(wxFONTWEIGHT_BOLD);
    title->SetFont(titleFont);

    controlStatusLabel_ = new wxStaticText(panel, wxID_ANY, "Scan stations to populate the list");
    controlStatusLabel_->SetForegroundColour(wxColour(95, 104, 116));

    auto *stationLabel = new wxStaticText(panel, wxID_ANY, "Station");
    stationChoice_ = new wxChoice(panel, wxID_ANY);

    auto *buttonRow = new wxBoxSizer(wxHORIZONTAL);
    startButton_ = new wxButton(panel, wxID_ANY, "Start");
    stopButton_ = new wxButton(panel, wxID_ANY, "Stop");
    auto *scanButton = new wxButton(panel, wxID_ANY, "Scan");
    scanButton->SetMinSize(wxSize(120, 38));
    startButton_->SetMinSize(wxSize(120, 38));
    stopButton_->SetMinSize(wxSize(120, 38));

    buttonRow->Add(startButton_, 0, wxRIGHT, 10);
    buttonRow->Add(stopButton_, 0, wxRIGHT, 10);
    buttonRow->Add(scanButton, 0);

    scanButton->Bind(wxEVT_BUTTON, &MainFrame::OnScanButton, this);
    startButton_->Bind(wxEVT_BUTTON, &MainFrame::OnStartButton, this);
    stopButton_->Bind(wxEVT_BUTTON, &MainFrame::OnStopButton, this);
    stationChoice_->Bind(wxEVT_CHOICE, &MainFrame::OnStationSelected, this);

    controls->Add(title, 0, wxBOTTOM, 4);
    controls->Add(controlStatusLabel_, 0, wxBOTTOM, 14);
    controls->Add(stationLabel, 0, wxBOTTOM, 6);
    controls->Add(stationChoice_, 0, wxEXPAND | wxBOTTOM, 12);
    controls->Add(buttonRow, 0);

    content->Add(controls, 1, wxEXPAND | wxRIGHT, 24);
    if (showReceiverSection) {
        auto *receiverPanel = new wxBoxSizer(wxVERTICAL);
        auto *receiverTitle = new wxStaticText(panel, wxID_ANY, "Receiver");

        receiverTitle->SetFont(titleFont);
        receiverStatusLabel_ = new wxStaticText(panel, wxID_ANY, "Receiver: idle");
        receiverStatusLabel_->SetForegroundColour(wxColour(95, 104, 116));
        receiverStatusLabel_->SetMinSize(wxSize(300, -1));
        receiverStatusLabel_->Wrap(300);

        receiverPanel->Add(receiverTitle, 0, wxBOTTOM, 4);
        receiverPanel->Add(receiverStatusLabel_, 0, wxTOP, 2);
        receiverPanel->AddStretchSpacer(1);
        content->Add(receiverPanel, 1, wxEXPAND);
    }

    layout->Add(content, 1, wxEXPAND | wxTOP | wxRIGHT | wxBOTTOM, 18);
    panel->SetSizer(layout);
    return panel;
}

void MainFrame::OnScanButton(wxCommandEvent &) {
    if (dab_gui_playback_is_running() != 0) {
        if (controlStatusLabel_ != nullptr) {
            controlStatusLabel_->SetLabel("Stopping playback before scan...");
        }
        StopPlayback();
    }

    ScanDialog dialog(this, scanResultsPath_);

    if (dialog.ShowModal() == wxID_OK && dialog.ScanSucceeded()) {
        ReloadStationsFromScanResults();
        if (controlStatusLabel_ != nullptr) {
            controlStatusLabel_->SetLabel("Scan complete. Select a station and press Start.");
        }
        UpdatePlaybackControls();
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

void MainFrame::OnSdrSettings(wxCommandEvent &) {
    SdrSettingsDialog dialog(this, sdrSettings_);

    if (dialog.ShowModal() != wxID_OK) {
        return;
    }

    const bool wasPlaying = dab_gui_playback_is_running() != 0;
    sdrSettings_ = dialog.Settings();
    ApplySdrSettings();
    SaveSdrSettings();

    if (wasPlaying) {
        StopPlayback();
        if (controlStatusLabel_ != nullptr) {
            controlStatusLabel_->SetLabel("SDR settings saved. Restart playback to use them.");
        }
    } else if (controlStatusLabel_ != nullptr) {
        controlStatusLabel_->SetLabel(wxString::Format("SDR settings: %s", SdrSettingsSummary()));
    }
    UpdatePlaybackControls();
}

void MainFrame::OnAudioSettings(wxCommandEvent &) {
    AudioSettingsDialog dialog(this, audioSettings_);

    if (dialog.ShowModal() != wxID_OK) {
        return;
    }

    const bool wasPlaying = dab_gui_playback_is_running() != 0;
    audioSettings_ = dialog.Settings();
    ApplyAudioSettings();
    SaveAudioSettings();

    if (wasPlaying) {
        StopPlayback();
        if (controlStatusLabel_ != nullptr) {
            controlStatusLabel_->SetLabel("Audio settings saved. Press Start to resume playback.");
        }
    } else if (controlStatusLabel_ != nullptr) {
        controlStatusLabel_->SetLabel(wxString::Format("Audio backend: %s", AudioSettingsSummary()));
    }
    UpdatePlaybackControls();
}

void MainFrame::OnSpectrumSettings(wxCommandEvent &) {
    SpectrumSettingsDialog dialog(this, static_cast<SpectrumDisplayMode>(spectrumDisplayMode_));

    if (dialog.ShowModal() != wxID_OK) {
        return;
    }

    gSpectrumDisplayMode = dialog.DisplayMode();
    spectrumDisplayMode_ = static_cast<int>(gSpectrumDisplayMode);
    SaveSpectrumSettings();
    Refresh(false);
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

void MainFrame::OnPlaybackStatusTimer(wxTimerEvent &) {
    RefreshPlaybackStatus();
}

void MainFrame::RefreshPlaybackStatus() {
    dab_gui_playback_status_t status{};
    dab_gui_playback_logo_t logo{};
    auto *stationLogoPanel = dynamic_cast<StationLogoPanel *>(stationLogoPanel_);
    const int receiverWrapWidth = receiverStatusLabel_ != nullptr
                                      ? std::max(220, receiverStatusLabel_->GetClientSize().GetWidth())
                                      : 0;

    if (dab_gui_playback_get_status(&status) != 0) {
        return;
    }

    if (receiverWrapWidth > 0) {
        receiverStatusLabel_->Wrap(receiverWrapWidth);
    }

    if (!status.running) {
        if (receiverStatusLabel_ != nullptr) {
            receiverStatusLabel_->SetLabel("Receiver: idle");
        }
        if (stationLogoPanel != nullptr && lastLogoGeneration_ != 0u) {
            stationLogoPanel->ClearLogo();
            lastLogoGeneration_ = 0u;
        }
        return;
    }

    if (stationLogoPanel != nullptr &&
        dab_gui_playback_get_logo(&logo) == 0 &&
        logo.valid != 0 &&
        logo.generation != lastLogoGeneration_ &&
        logo.byte_count > 0u) {
        wxMemoryInputStream stream(logo.bytes, logo.byte_count);
        const wxBitmapType bitmapType = DetectBitmapType(logo.bytes, logo.byte_count);
        wxLogNull suppressImageErrors;
        wxImage image(stream, bitmapType);

        if (image.IsOk()) {
            stationLogoPanel->SetLogoImage(image);
            lastLogoGeneration_ = logo.generation;
        }
    } else if (stationLogoPanel != nullptr && logo.valid == 0 && lastLogoGeneration_ != 0u) {
        stationLogoPanel->ClearLogo();
        lastLogoGeneration_ = 0u;
    }

    if (receiverStatusLabel_ == nullptr) {
        return;
    }

    if (!status.locked) {
        receiverStatusLabel_->SetLabel(wxString::Format("Receiver: acquiring, PRS %.3f", status.prs_score));
        return;
    }

    if (status.has_receiver_snr || status.has_spectrum_cn || status.has_rf_level) {
        wxString label = "Receiver: locked";
        if (status.has_rf_level) {
            label += wxString::Format(", RF %.1f dBFS", status.rf_level_dbfs);
        }
        if (status.has_receiver_snr) {
            label += wxString::Format(", SNR %.1f dB", status.receiver_snr_db);
        }
        if (status.has_spectrum_cn) {
            label += wxString::Format(", C/N %.1f dB", status.spectrum_cn_db);
        }
        label += wxString::Format(", PRS %.3f", status.prs_score);
        receiverStatusLabel_->SetLabel(label);
        return;
    }

    receiverStatusLabel_->SetLabel(wxString::Format("Receiver: locked, estimating SNR, PRS %.3f",
                                                    status.prs_score));
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
    if (auto *stationLogoPanel = dynamic_cast<StationLogoPanel *>(stationLogoPanel_)) {
        stationLogoPanel->ClearLogo();
        lastLogoGeneration_ = 0u;
    }
    if (controlStatusLabel_ != nullptr) {
        controlStatusLabel_->SetLabel("Playback stopped.");
    }
    UpdatePlaybackControls();
}
