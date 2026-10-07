#include "pch.h"
#include "App.xaml.h"
#include "MainWindow.xaml.h"

using namespace winrt;
using namespace Microsoft::UI::Xaml;

namespace winrt::DabMeasurementWinUI::implementation
{
    App::App()
    {
#if defined(_DEBUG) && !defined(DISABLE_XAML_GENERATED_BREAK_ON_UNHANDLED_EXCEPTION)
        UnhandledException([](IInspectable const&, UnhandledExceptionEventArgs const& event)
        {
            if (IsDebuggerPresent())
            {
                auto message = event.Message();
                (void)message;
                __debugbreak();
            }
        });
#endif
    }

    void App::OnLaunched([[maybe_unused]] LaunchActivatedEventArgs const& event)
    {
        m_window = make<MainWindow>();
        m_window.Activate();
    }
}
