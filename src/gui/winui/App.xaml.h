#pragma once

#include "App.xaml.g.h"
#include "main_window.hpp"

namespace winrt::dab_gui::implementation {

struct App : AppT<App> {
    App();
    void OnLaunched(Microsoft::UI::Xaml::LaunchActivatedEventArgs const &args);

private:
    std::shared_ptr<MainWindow> window_;
};

}
