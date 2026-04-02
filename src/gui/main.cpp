#include "main_frame.hpp"

#include <wx/app.h>
#include <wx/image.h>

class DabGuiApp : public wxApp {
public:
    bool OnInit() override {
        wxInitAllImageHandlers();
        auto *frame = new MainFrame();
        frame->Show(true);
        return true;
    }
};

wxIMPLEMENT_APP(DabGuiApp);
