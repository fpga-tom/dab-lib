#include "main_frame.hpp"

#include <wx/app.h>

class DabGuiApp : public wxApp {
public:
    bool OnInit() override {
        auto *frame = new MainFrame();
        frame->Show(true);
        return true;
    }
};

wxIMPLEMENT_APP(DabGuiApp);
