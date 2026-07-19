# DAB+ Measurement Workbench — WinUI proof of concept

This is a Windows-only C++/WinUI 3 shell for the measurement system. It is deliberately separate from the existing wxWidgets receiver GUI.

The current proof of concept includes:

- receiver controls for device, the complete 5A–13F Band III channel plan, mapped centre frequency, gain, start, record, and markers;
- live ensemble, service, and component navigation decoded from the tuned signal's FIC;
- live spectrum, rolling waterfall, constellation, CIR, and per-carrier MER views;
- a full-tap CIR diagnostic with Hann-windowed channel estimation, an exact 8-frame linear-power average, relative-delay/level axes, adaptive floor estimation, separated strongest/candidate-path markers, and an in-window maximize/restore view (`Ctrl+M`);
- live KPI cards and receiver detail values;
- a dedicated RF Measurements workspace with toolbar, navigation, View-menu, and live-summary entry points;
- an RF Overview page with live ADC-referenced power, C/N, carrier MER, CFO, synchronization stability, overruns, receiver configuration, process CPU usage, and rolling 60-second traces;
- an RF Spectrum page driven by the native 2048-bin/1 kHz receiver FFT, with display averaging, peak hold, reference and scale controls, waterfall, dual/delta markers, cursor readout, wheel zoom, drag pan, double-click reset, freeze, tune-at-cursor, and PNG capture to the Pictures library;
- event/annotation timeline and receiver status bar;
- a thread-safe latest-snapshot mailbox compatible with `dab_receive_config_t.snapshot`;
- a UI-owned receiver worker that runs `dab_receive_service` on a dedicated thread;
- cooperative Start/Stop, receiver state, and error reporting; and
- live ADC, PRS, C/N, CFO, timing, drop, FIC, and MSC snapshot values.
- a separate bounded high-rate measurement mailbox for decoder-owned array payloads.
- a deduplicated catalog mailbox carrying owned FIC ensemble, service, component, subchannel, and protection metadata.

`ReceiverWorker` owns the synchronous receiver session and connects its snapshot callback to the mailbox:

```cpp
config.snapshot = &dab::measurement::SnapshotMailbox::Callback;
config.snapshot_user = &mailbox;
config.measurement = &dab::measurement::MeasurementMailbox::Callback;
config.measurement_user = &measurement_mailbox;
config.measurement_decimation = 1;
config.catalog = &dab::measurement::CatalogMailbox::Callback;
config.catalog_user = &catalog_mailbox;
```

The scalar snapshot remains lightweight. The measurement callback exposes temporary views of 2048 FFT-shifted spectrum bins, up to 512 normalized constellation points, 512 CIR taps, and 1536 physical-carrier MER values. `MeasurementMailbox` copies the latest complete transmission-frame payload before the callback returns. The catalog callback is emitted only when the decoded FIC repository changes and is copied by `CatalogMailbox`. The UI timer therefore never touches decoder-owned memory, and the receiver thread never updates XAML controls directly.

The receiver controls currently open a real SDR device. The selected SDRplay or RTL-SDR adapter must be installed and available; explicitly selecting a real device no longer falls back silently to the synthetic adapter. On Windows, the SDRplay loader also checks the standard API installation directories.

Current proof-of-concept limitations:

- selecting an audio service in the navigation list restarts the receiver session on the same channel with that explicit SID;
- absolute input power remains uncalibrated, so RF power is correctly displayed in dBFS rather than dBm; sample-clock and ADC-clipping estimators are not available yet;
- the high-rate callback is emitted after a complete synchronized DAB transmission frame; acquisition without frame synchronization produces scalar snapshots but no array payload;
- CIR delays are relative to the receiver FFT timing reference and are not absolute transmitter propagation delays;
- FFT size and window controls describe the native receiver FFT and are read-only; triggered raw-IQ capture remains disabled until the receiver pipeline exposes a raw sample recorder;
- BER remains a FIC CRC-loss proxy; and
- recording is not implemented, so the Record control is disabled.

## Build

Requirements:

- Visual Studio 2022 with Desktop development with C++ and WinUI application development;
- Windows 10 version 1809 or newer;
- NuGet access for the pinned `Microsoft.WindowsAppSDK` package.

From a Visual Studio developer PowerShell:

```powershell
msbuild .\apps\dab_measurement_winui\DabMeasurementWinUI.sln /restore /p:Configuration=Debug /p:Platform=x64
```

Generated files and binaries are placed under `out\winui`.
