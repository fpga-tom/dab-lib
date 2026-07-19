#pragma once

#include "CatalogMailbox.h"
#include "MainWindow.g.h"
#include "MeasurementMailbox.h"
#include "ReceiverWorker.h"
#include "SnapshotMailbox.h"

namespace winrt::DabMeasurementWinUI::implementation
{
    struct MainWindow : MainWindowT<MainWindow>
    {
        MainWindow();
        ~MainWindow();

        void StartButton_Click(IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void ChannelComboBox_SelectionChanged(IInspectable const&, Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const&);
        void ServicesListView_SelectionChanged(IInspectable const&, Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const&);
        void RecordButton_Click(IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void MarkerButton_Click(IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void Window_Closed(IInspectable const&, Microsoft::UI::Xaml::WindowEventArgs const&);

    private:
        void OnTimerTick(IInspectable const&, IInspectable const&);
        std::optional<dab::measurement::ReceiverSettings> ReadReceiverSettings();
        void HandleReceiverStatus(dab::measurement::ReceiverStatus const& status);
        void SetReceiverControlsEnabled(bool enabled);
        void ApplySnapshot(dab_receive_snapshot_t const& snapshot);
        void ApplyCatalog(dab_receive_catalog_t const& catalog);
        void ApplySelectedServiceComponents();
        void ClearCatalogView();
        void UpdatePlots(dab::measurement::MeasurementFrame const& frame);
        void AddTimelineEvent(winrt::hstring const& message);

        Microsoft::UI::Xaml::DispatcherTimer m_timer;
        dab::measurement::SnapshotMailbox m_mailbox;
        dab::measurement::MeasurementMailbox m_measurement_mailbox;
        dab::measurement::CatalogMailbox m_catalog_mailbox;
        dab::measurement::ReceiverWorker m_receiver{
            m_mailbox,
            m_measurement_mailbox,
            m_catalog_mailbox
        };
        dab_receive_catalog_t m_latest_catalog{};
        bool m_have_catalog{ false };
        uint64_t m_mailbox_version{ 0 };
        uint64_t m_measurement_mailbox_version{ 0 };
        uint64_t m_catalog_mailbox_version{ 0 };
        uint64_t m_receiver_status_version{ 0 };
        uint64_t m_tick{ 0 };
    };
}

namespace winrt::DabMeasurementWinUI::factory_implementation
{
    struct MainWindow : MainWindowT<MainWindow, implementation::MainWindow>
    {
    };
}
