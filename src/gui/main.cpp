#include "main_frame.hpp"

#include <wx/app.h>
#include <wx/image.h>

class DabGuiApp : public wxApp {
public:
    bool OnInit() override {
        bool showEnsembleTime = false;
        for (int index = 1; index < argc; ++index) {
            if (wxString(argv[index]) == "--show-ensemble-time") {
                showEnsembleTime = true;
                break;
            }
        }

        wxInitAllImageHandlers();
        auto *frame = new MainFrame(showEnsembleTime);
        frame->Show(true);
        return true;
    }
};

wxIMPLEMENT_APP(DabGuiApp);
