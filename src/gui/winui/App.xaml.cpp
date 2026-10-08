#include "App.xaml.h"

#include <windows.h>

namespace winrt::dab_gui::implementation {

App::App() {
    InitializeComponent();
}

void App::OnLaunched(Microsoft::UI::Xaml::LaunchActivatedEventArgs const &) {
    const std::wstring commandLine = GetCommandLineW();
    window_ = std::make_shared<MainWindow>(
        commandLine.find(L"--show-ensemble-time") != std::wstring::npos);
    window_->Activate();
}

}
