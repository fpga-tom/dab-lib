#pragma once

#include "CatalogMailbox.h"
#include "MainWindow.g.h"
#include "MeasurementMailbox.h"
#include "ReceiverWorker.h"
#include "SnapshotMailbox.h"
#include "dab/measurement_alarm.h"

namespace winrt::DabMeasurementWinUI::implementation
{
    struct MainWindow : MainWindowT<MainWindow>
    {
        MainWindow();
        ~MainWindow();

        void StartButton_Click(IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void ChannelComboBox_SelectionChanged(IInspectable const&, Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const&);
        void ServicesListView_SelectionChanged(IInspectable const&, Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const&);
        void ServicesListView_ItemClick(IInspectable const&, Microsoft::UI::Xaml::Controls::ItemClickEventArgs const&);
        void ReceiverWorkspace_Click(IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void RfMeasurements_Click(IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void RootLayout_SizeChanged(IInspectable const&, Microsoft::UI::Xaml::SizeChangedEventArgs const&);
        void RfWorkspace_SizeChanged(IInspectable const&, Microsoft::UI::Xaml::SizeChangedEventArgs const&);
        void CirExpandButton_Click(IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void RfSpectrum_Loaded(IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void RfSpectrumControls_SelectionChanged(IInspectable const&, Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const&);
        void RfSpectrumCenter_ValueChanged(IInspectable const&, Microsoft::UI::Xaml::Controls::NumberBoxValueChangedEventArgs const&);
        void RfSpectrumReference_ValueChanged(IInspectable const&, Microsoft::UI::Xaml::Controls::NumberBoxValueChangedEventArgs const&);
        void RfSpectrumMarker_Click(IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void RfSpectrumPeakHold_Click(IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void RfSpectrumClearPeaks_Click(IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void RfSpectrumFreeze_Click(IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        winrt::fire_and_forget RfSpectrumSaveScreenshot_Click(IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void RfSpectrumCanvas_PointerWheelChanged(IInspectable const&, Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const&);
        void RfSpectrumCanvas_PointerPressed(IInspectable const&, Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const&);
        void RfSpectrumCanvas_PointerMoved(IInspectable const&, Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const&);
        void RfSpectrumCanvas_PointerReleased(IInspectable const&, Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const&);
        void RfSpectrumCanvas_PointerExited(IInspectable const&, Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const&);
        void RfSpectrumCanvas_PointerCaptureLost(IInspectable const&, Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const&);
        void RfSpectrumCanvas_DoubleTapped(IInspectable const&, Microsoft::UI::Xaml::Input::DoubleTappedRoutedEventArgs const&);
        void RfSpectrumTuneToCursor_Click(IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void RfSpectrumResetView_Click(IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void RfChannelCirCanvas_PointerPressed(IInspectable const&, Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const&);
        void RfHistory_Loaded(IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void RfHistoryPeriod_SelectionChanged(IInspectable const&, Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const&);
        void RfHistoryControls_Changed(IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void RfHistoryPause_Click(IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void RfHistoryAnnotationCommit_Click(IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        winrt::fire_and_forget RfHistoryExportCsv_Click(IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void RfHistoryClearEvents_Click(IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void RfHistoryAlarmMeasurement_SelectionChanged(IInspectable const&, Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const&);
        void RfHistoryAlarmApply_Click(IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void RfHistoryAlarmEnable_Click(IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void AudioButton_Click(IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void RecordButton_Click(IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void MarkerButton_Click(IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void Window_Closed(IInspectable const&, Microsoft::UI::Xaml::WindowEventArgs const&);

    private:
        void OnTimerTick(IInspectable const&, IInspectable const&);
        std::optional<dab::measurement::ReceiverSettings> ReadReceiverSettings();
        void HandleReceiverStatus(dab::measurement::ReceiverStatus const& status);
        void SetReceiverControlsEnabled(bool enabled);
        void ApplySnapshot(dab_receive_snapshot_t const& snapshot);
        void ApplyRfSnapshot(dab_receive_snapshot_t const& snapshot);
        void UpdateRfInputHistogram(dab_receive_snapshot_t const& snapshot);
        void ApplyRfSyncSnapshot(dab_receive_snapshot_t const& snapshot);
        void AppendRfSyncHistory(dab_receive_snapshot_t const& snapshot);
        void UpdateRfSyncHistoryVisuals();
        void ResetRfSyncDiagnostics();
        void AddRfSyncEvent(winrt::hstring const& message);
        void AppendRfHistory(float power_dbfs, float cn_db);
        void UpdateRfHistoryVisuals();
        void UpdateRfContext();
        void ShowWorkspace(bool rf_measurements);
        void SetCirPanelMaximized(bool maximized);
        void ApplyWorkspaceWidth(double root_width);
        void UpdateCpuUsage();
        void ApplyCatalog(dab_receive_catalog_t const& catalog);
        void ApplySelectedServiceComponents();
        void SelectAudioService(uint32_t service_id);
        bool StartReceiver(dab::measurement::ReceiverSettings const& settings,
                           bool clear_catalog,
                           winrt::hstring const& timeline_message);
        void StartPendingServiceRestart();
        void ClearCatalogView();
        void UpdatePlots(dab::measurement::MeasurementFrame const& frame);
        void UpdateCirPlot(dab::measurement::MeasurementFrame const& frame);
        void UpdateRfChannel(dab::measurement::MeasurementFrame const& frame);
        void UpdateRfChannelResponse(dab::measurement::MeasurementFrame const& frame);
        void UpdateRfChannelCir(dab::measurement::MeasurementFrame const& frame);
        void RenderRfChannelCirMarker();
        void ResetRfChannelDiagnostics();
        void AppendRfLongHistory(dab_receive_snapshot_t const& snapshot);
        void UpdateRfLongHistoryVisuals();
        void ResetRfHistorySession();
        void EvaluateRfAlarm(dab_receive_snapshot_t const& snapshot);
        bool ApplyRfAlarmConfiguration(bool add_event);
        void AddRfHistoryEvent(winrt::hstring const& severity,
                               winrt::hstring const& measurement,
                               winrt::hstring const& value,
                               winrt::hstring const& description,
                               uint64_t timestamp_ns = 0u);
        void RefreshRfHistoryEventList();
        uint64_t RfHistoryPeriodNs();
        void UpdateRfSpectrum(dab::measurement::MeasurementFrame const& frame);
        void RenderRfSpectrum();
        void AppendRfSpectrumWaterfall();
        void ResetRfSpectrumView();
        void ClampRfSpectrumView();
        void UpdateRfSpectrumReadouts();
        void UpdateRfSpectrumHover(Windows::Foundation::Point const& position);
        void SetRfSpectrumMarker(uint32_t marker, double frequency_hz);
        double RfSpectrumFrequencyFromX(double x) const noexcept;
        double RfSpectrumXFromFrequency(double frequency_hz) const noexcept;
        float RfSpectrumPowerAtFrequency(double frequency_hz) const noexcept;
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
        bool m_metrics_stale{ false };
        uint32_t m_selected_service_id{ 0u };
        uint32_t m_active_service_id{ 0u };
        std::optional<dab::measurement::ReceiverSettings> m_pending_receiver_restart;
        static constexpr size_t rf_history_capacity = 240u;
        std::array<float, rf_history_capacity> m_rf_power_history{};
        std::array<float, rf_history_capacity> m_rf_cn_history{};
        std::array<float, rf_history_capacity> m_rf_mer_history{};
        size_t m_rf_history_write{ 0u };
        size_t m_rf_history_count{ 0u };
        float m_latest_mer_db{ 0.0f };
        bool m_latest_mer_valid{ false };
        bool m_rf_workspace_active{ false };
        bool m_cir_panel_maximized{ false };
        bool m_rf_was_locked{ false };
        uint64_t m_rf_lock_started_ns{ 0u };
        static constexpr size_t rf_sync_history_capacity = 240u;
        std::array<float, rf_sync_history_capacity> m_rf_sync_coarse_history{};
        std::array<float, rf_sync_history_capacity> m_rf_sync_fine_history{};
        std::array<float, rf_sync_history_capacity> m_rf_sync_residual_history{};
        std::array<float, rf_sync_history_capacity> m_rf_sync_clock_history{};
        std::array<uint64_t, rf_sync_history_capacity> m_rf_sync_timestamp_history{};
        size_t m_rf_sync_history_write{ 0u };
        size_t m_rf_sync_history_count{ 0u };
        uint32_t m_rf_sync_last_loss_count{ 0u };
        uint32_t m_rf_sync_last_reacquisition_count{ 0u };
        bool m_rf_sync_seen_lock{ false };
        uint64_t m_last_process_time_100ns{ 0u };
        uint64_t m_last_system_time_100ns{ 0u };
        static constexpr size_t cir_average_capacity = 8u;
        std::array<std::array<float, DAB_RECEIVE_CIR_BIN_COUNT>, cir_average_capacity>
            m_cir_power_history_linear{};
        std::array<float, DAB_RECEIVE_CIR_BIN_COUNT> m_cir_power_sum_linear{};
        size_t m_cir_power_history_write{ 0u };
        size_t m_cir_power_history_count{ 0u };
        uint64_t m_cir_frequency_hz{ 0u };
        uint32_t m_cir_bin_count{ 0u };
        float m_cir_sample_period_ns{ 0.0f };
        std::array<float, DAB_RECEIVE_CIR_BIN_COUNT> m_rf_channel_cir_level_db{};
        uint32_t m_rf_channel_cir_count{ 0u };
        uint32_t m_rf_channel_cir_main_bin{ 0u };
        float m_rf_channel_cir_sample_period_ns{ 0.0f };
        int32_t m_rf_channel_cir_marker_delay_samples{ -1 };
        struct RfHistorySample
        {
            uint64_t timestamp_ns{ 0u };
            uint64_t wall_time_100ns{ 0u };
            float power_dbfs{ 0.0f };
            float mer_db{ 0.0f };
            float cfo_hz{ 0.0f };
            float clipping_percent{ 0.0f };
            bool power_valid{ false };
            bool mer_valid{ false };
            bool cfo_valid{ false };
            bool clipping_valid{ false };
            bool locked{ false };
        };
        struct RfHistoryEvent
        {
            uint64_t timestamp_ns{ 0u };
            uint64_t wall_time_100ns{ 0u };
            std::wstring severity;
            std::wstring measurement;
            std::wstring value;
            std::wstring description;
        };
        static constexpr size_t rf_long_history_capacity = 14400u;
        std::array<RfHistorySample, rf_long_history_capacity> m_rf_long_history{};
        size_t m_rf_long_history_write{ 0u };
        size_t m_rf_long_history_count{ 0u };
        std::vector<RfHistoryEvent> m_rf_history_events;
        dab_measurement_alarm_state_t m_rf_alarm{};
        int32_t m_rf_alarm_measurement_index{ 0 };
        float m_rf_alarm_last_value{ 0.0f };
        bool m_rf_alarm_last_value_valid{ false };
        bool m_rf_alarm_enabled{ true };
        bool m_rf_history_controls_ready{ false };
        bool m_rf_history_display_paused{ false };
        uint64_t m_rf_history_pause_timestamp_ns{ 0u };
        std::array<float, DAB_RECEIVE_SPECTRUM_BIN_COUNT> m_rf_spectrum_average{};
        std::array<float, DAB_RECEIVE_SPECTRUM_BIN_COUNT> m_rf_spectrum_peak{};
        uint32_t m_rf_spectrum_count{ 0u };
        uint32_t m_rf_spectrum_sample_rate_hz{ 0u };
        uint32_t m_rf_spectrum_average_factor{ 8u };
        double m_rf_spectrum_source_center_hz{ 213360000.0 };
        double m_rf_spectrum_source_start_hz{ 212336000.0 };
        double m_rf_spectrum_bin_hz{ 1000.0 };
        double m_rf_spectrum_view_center_hz{ 213360000.0 };
        double m_rf_spectrum_view_span_hz{ 2048000.0 };
        double m_rf_spectrum_reference_dbfs{ -20.0 };
        double m_rf_spectrum_scale_db{ 10.0 };
        double m_rf_spectrum_cursor_frequency_hz{ 213360000.0 };
        std::array<double, 2> m_rf_spectrum_marker_frequency_hz{};
        std::array<float, 2> m_rf_spectrum_marker_power_dbfs{};
        std::array<bool, 2> m_rf_spectrum_marker_valid{};
        Windows::Foundation::Point m_rf_spectrum_pointer_start{};
        double m_rf_spectrum_pan_start_center_hz{ 0.0 };
        uint32_t m_rf_spectrum_active_marker{ 0u };
        int32_t m_rf_spectrum_drag_marker{ -1 };
        bool m_rf_spectrum_controls_ready{ false };
        bool m_rf_spectrum_updating_controls{ false };
        bool m_rf_spectrum_average_initialized{ false };
        bool m_rf_spectrum_peak_hold{ true };
        bool m_rf_spectrum_frozen{ false };
        bool m_rf_spectrum_pointer_pressed{ false };
        bool m_rf_spectrum_pointer_moved{ false };
    };
}

namespace winrt::DabMeasurementWinUI::factory_implementation
{
    struct MainWindow : MainWindowT<MainWindow, implementation::MainWindow>
    {
    };
}
