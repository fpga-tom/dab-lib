# DAB+ Measurement Workbench — WinUI proof of concept

This is a Windows-only C++/WinUI 3 shell for the measurement system. It is deliberately separate from the existing wxWidgets receiver GUI.

The current proof of concept includes:

- receiver controls for device, the complete 5A–13F Band III channel plan, mapped centre frequency, gain, start, record, and markers;
- ensemble, service, and component navigation;
- spectrum, waterfall, constellation, CIR, and carrier-MER placeholders;
- live KPI cards and receiver detail values;
- event/annotation timeline and receiver status bar;
- a thread-safe latest-snapshot mailbox compatible with `dab_receive_config_t.snapshot`;
- a UI-owned receiver worker that runs `dab_receive_service` on a dedicated thread;
- cooperative Start/Stop, receiver state, and error reporting; and
- live ADC, PRS, C/N, CFO, timing, drop, FIC, and MSC snapshot values.

`ReceiverWorker` owns the synchronous receiver session and connects its snapshot callback to the mailbox:

```cpp
config.snapshot = &dab::measurement::SnapshotMailbox::Callback;
config.snapshot_user = &mailbox;
```

The UI timer reads the newest copied snapshot without touching decoder-owned memory. Receiver status events are copied through a second thread-safe slot, so the receiver thread never updates XAML controls directly.

The receiver controls currently open a real SDR device. The selected SDRplay or RTL-SDR adapter must be installed and available; explicitly selecting a real device no longer falls back silently to the synthetic adapter. On Windows, the SDRplay loader also checks the standard API installation directories.

Current proof-of-concept limitations:

- the service ID is currently seeded with `0x00001001`; the reusable receiver session falls back to the first service discovered from FIC after its acquisition interval;
- spectrum, waterfall, constellation, CIR, and carrier-MER panels remain visual placeholders because the snapshot contract does not yet carry raw bins, symbols, or impulse-response arrays;
- MER is displayed as a clearly labelled C/N-derived proxy and BER as a FIC CRC-loss proxy; and
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
