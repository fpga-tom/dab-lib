# dab-lib

Copyright (C) 2026 Tomas Krajco

Plain-C DAB/DAB+ receiver skeleton with:
- portable SDR HAL
- portable OS layer
- SIMD dispatch
- block-based threaded sample flow
- first frontend stages for IQ conversion, DC removal, and null-symbol detection

## Build

```sh
cmake -S . -B build
cmake --build build
```

Pure `Release` builds use speed optimization, aggressive inlining, and
interprocedural/link-time optimization when the compiler supports it. Set
`-DDAB_ENABLE_RELEASE_IPO=OFF` to disable the latter for troubleshooting.

Live SDR streams preallocate 32 IQ capture blocks by default (about 2 MiB per
open stream). Builds that need a larger capture latency cushion can override
the depth, for example `cmake -S . -B build -DDAB_CAPTURE_BLOCK_COUNT=64`.

## Windows Installer

To stage the GUI application and build an installer on Windows:

```powershell
cmake --build build-win --config Release --target dab_gui
cmake --install build-win --config Release --prefix dist/install/Release
powershell -ExecutionPolicy Bypass -File .\installer\build_installer.ps1 -Config Release
```

This packages:
- `dab_gui.exe`
- the required wxWidgets runtime DLLs
- `portaudio_x64.dll`
- `libfaad2_dll.dll`
- `INSTALL.md`

The Inno Setup script is:
- `installer/dab_gui.iss`

The generated installer is written to:
- `dist/installer`

The installer includes the project license and notices for bundled third-party
libraries.

## License

DAB SDR GUI and dab-lib are free software licensed under the GNU General Public
License, version 2 or (at your option) any later version. See `LICENSE`.

Bundled dependencies retain their own licenses and copyright notices. See
`THIRD_PARTY_NOTICES.md`.

## Tests

```sh
./build/dab_tests
ctest --test-dir build --output-on-failure
```

The SIMD conversion microbenchmark accepts an optional iteration count:

```sh
./build/dab_simd_bench 20000
```

## SDRplay Probe

One-shot real-hardware sanity probe:

```sh
DAB_SDRPLAY_USE_REAL=1 ./build/dab_sdrplay_probe
```

Useful debug env vars:
- `DAB_SDRPLAY_DEBUG=1`
- `DAB_SDRPLAY_STARTUP_UPDATE=1`

Note:
- real SDRplay runs must be executed outside restrictive sandboxes because the SDRplay runtime uses shared memory
- the current real path is validated against SDRplay API `3.15`

## Live Audio Playback

Real-hardware over-the-air DAB+ capture, decode, and audio playback:

```sh
DAB_SDRPLAY_USE_REAL=1 ./build/dab_sdrplay_audio --freq 213360000 --agc on
```

This tool:
- opens the SDRplay device
- tunes to the requested Band III center frequency
- enables or disables SDR AGC from the command line
- captures live IQ
- runs the current sync, FIC, and MSC decode path
- auto-selects the first decodable audio service it finds
- decodes DAB+ audio and plays it through PortAudio
- attempts recovery after lock loss or prolonged no-audio periods

Notes:
- `--freq` is required and expects Hz
- `--agc` accepts `on` or `off`
- `--sid` optionally locks playback to one service ID, in decimal or hex like `0x1234`
- `--debug` toggles verbose sync/decode logs with `on` or `off`
- `DAB_SDRPLAY_AUDIO_RESET_TIMEOUT_SECONDS` controls how long the tool waits before forcing audio recovery reset
- stop playback with `Ctrl+C`
- the build needs `portaudio-2.0` plus either `fdk-aac` or `faad2`

Status lines:
- `service-discovered sid=...` means the selected service is present in FIC/MSC metadata
- `waiting-for-audio sid=...` means sync is back but no valid DAB+ superframe is available yet
- `playing sid=...` means audio superframes are flowing again and playback resumed
- `lost-lock ...`, `audio-timeout ...`, and `audio-recovery-reset ...` describe the current recovery path

Decoder note:
- `fdk-aac` is preferred when available
- `faad2` builds work, but some stations can still hit AU decode failures such as unsupported gain-control features

## Null-Symbol Debug Probe

Real-hardware raw power-drop logging over the current frontend pipeline:

```sh
DAB_SDRPLAY_USE_REAL=1 ./build/dab_sdrplay_null_probe
```

This probe:
- opens the SDRplay device
- configures the stream for DAB Band III defaults
- reads IQ blocks
- runs frontend IQ conversion + DC removal
- runs a rolling fast/slow power ratio drop detector
- prints drop starts, drop ends, and completed drops by default
- can optionally print periodic status lines

Example output:

```text
null-probe start sample_rate=2048000 center_freq_hz=220352000 bandwidth_hz=1536000 blocks=200 gain_mode=manual fast_alpha=0.0500 slow_alpha=0.0010 enter_ratio=0.3000 exit_ratio=0.5000 expected_null_interval=0
drop-start block=146 sample_index=196695 fast_power=148112.438 slow_power=1200444.500 ratio=0.123383
drop sample_index=196695 length=241 avg_power=12.441223 min_ratio=0.041122
drop-start block=292 sample_index=393307 fast_power=150331.125 slow_power=1210088.250 ratio=0.124230
drop sample_index=393307 length=239 avg_power=13.115002 min_ratio=0.039881
```

Useful env vars:
- `DAB_NULL_PROBE_BLOCKS`
- `DAB_NULL_PROBE_LOG_EVERY_BLOCKS`
- `DAB_NULL_PROBE_CENTER_FREQ_HZ`
- `DAB_NULL_PROBE_SAMPLE_RATE`
- `DAB_NULL_PROBE_BANDWIDTH_HZ`
- `DAB_NULL_PROBE_GAIN_DB`
- `DAB_NULL_PROBE_DC_ALPHA`
- `DAB_NULL_PROBE_FAST_ALPHA`
- `DAB_NULL_PROBE_SLOW_ALPHA`
- `DAB_NULL_PROBE_FAST_WINDOW_SAMPLES`
- `DAB_NULL_PROBE_SLOW_WINDOW_SAMPLES`
- `DAB_NULL_PROBE_SAMPLE_DECIMATION`
- `DAB_NULL_PROBE_GAIN_MODE` (`manual` or `auto`)
- `DAB_NULL_PROBE_ENTER_RATIO_THRESHOLD`
- `DAB_NULL_PROBE_EXIT_RATIO_THRESHOLD`
- `DAB_NULL_PROBE_MIN_NULL_SAMPLES`
- `DAB_NULL_PROBE_MAX_NULL_SAMPLES`
- `DAB_NULL_PROBE_STARTUP_IGNORE_SAMPLES`
- `DAB_NULL_PROBE_EXPECTED_NULL_INTERVAL`
- `DAB_NULL_PROBE_INTERVAL_TOLERANCE_SAMPLES`
- `DAB_NULL_PROBE_MAX_MERGE_GAP_SAMPLES`
- `DAB_NULL_PROBE_LNA_STATE`
- `DAB_NULL_PROBE_IF_GAIN_DB`
- `DAB_NULL_PROBE_BIAS_TEE`
- `DAB_NULL_PROBE_PPM`

Recommended first run:

```sh
DAB_SDRPLAY_USE_REAL=1 \
DAB_SDRPLAY_DEBUG=1 \
DAB_NULL_PROBE_CENTER_FREQ_HZ=213360000 \
DAB_NULL_PROBE_GAIN_MODE=manual \
DAB_NULL_PROBE_FAST_ALPHA=0.05 \
DAB_NULL_PROBE_SLOW_ALPHA=0.001 \
DAB_NULL_PROBE_ENTER_RATIO_THRESHOLD=0.30 \
DAB_NULL_PROBE_EXIT_RATIO_THRESHOLD=0.50 \
DAB_NULL_PROBE_BLOCKS=100 \
./build/dab_sdrplay_null_probe
```

If you want periodic status snapshots without logging every block:

```sh
DAB_SDRPLAY_USE_REAL=1 \
DAB_NULL_PROBE_CENTER_FREQ_HZ=213360000 \
DAB_NULL_PROBE_LOG_EVERY_BLOCKS=25 \
./build/dab_sdrplay_null_probe
```

The default probe mode is intentionally simple:
- no startup ignore
- no cadence gating
- no null-length gating
- no merge-gap logic
- only fast/slow power averages and enter/exit ratio control detection

The detector now uses true moving-average power:
- `fast_window_samples`: short boxcar average
- `slow_window_samples`: long boxcar average
- `sample_decimation`: optional grouped-power decimation factor
- `ratio = fast_power / slow_power`

If the window env vars are not set, the detector derives window sizes from the legacy alpha values for compatibility.

For terrestrial Band III DAB Mode I at `2.048 Msps`, if you later want to compare drop spacing against frame cadence, set:
- `DAB_NULL_PROBE_EXPECTED_NULL_INTERVAL=196608`
- `DAB_NULL_PROBE_INTERVAL_TOLERANCE_SAMPLES=12000`

Interpretation:
- `drop-start` shows the first sample where the fast/slow ratio crossed the enter threshold
- `drop` shows the completed drop length and the minimum observed fast/slow ratio inside it
- `summary` includes the total `dropped_blocks` count reported by the SDRplay stream bridge
- `delta` and `error` are printed only if `DAB_NULL_PROBE_EXPECTED_NULL_INTERVAL` is set

## Current Status

Implemented:
- SDRplay HAL with stub and real dynamic-loading path
- RTL-SDR HAL with stub and real dynamic-loading path
- frontend IQ conversion to planar float and DC removal
- rolling null-symbol detector
- phase-reference detection, coarse timing, and coarse CFO tracking
- OFDM differential demodulation and softbit generation
- FIC decode with service and label parsing
- MSC routing with DAB+ superframe assembly
- live SDRplay over-the-air audio playback via `dab_sdrplay_audio`
- recovery path for lock loss and prolonged no-audio conditions

Working well enough for:
- tuning one Band III ensemble from SDRplay hardware
- locking to one service with `--sid`
- recovering from at least some antenna-loss / signal-drop events

Current limitations:
- recovery is still heuristic, not a production-grade retune/state-machine yet
- `service-discovered` only means the service mapping exists; it does not guarantee playable audio yet
- some services remain decoder-limited when built against `faad2`; `fdk-aac` should be preferred
- the live tool is still SDRplay-specific even though most DSP stages are generic

## FIC Dump Debugging

You can dump one tracked FIC frame from the sync probe and analyze it offline.

Capture one FIC frame from live hardware:

```sh
DAB_SDRPLAY_USE_REAL=1 \
DAB_SYNC_PROBE_CENTER_FREQ_HZ=213360000 \
DAB_SYNC_PROBE_GAIN_MODE=manual \
DAB_SYNC_PROBE_DUMP_FIC_PATH=/tmp/fic_frame.bin \
./build-release/dab_sdrplay_sync_probe
```

When the first full frame completes, the probe writes raw FIC softbits to the path in
`DAB_SYNC_PROBE_DUMP_FIC_PATH`.

Then analyze the dump locally:

```sh
./build/dab_fic_dump_analyze /tmp/fic_frame.bin
```

The analyzer tries a few likely symbol-layout interpretations:
- planar `a[0..1535], b[0..1535]`
- interleaved `a0,b0,a1,b1,...`
- swapped `a` / `b` halves
- each of the above with inverted softbit polarity

For each variant it reports:
- total valid FIB CRCs
- per-block best polarity / byte-pack convention seen by the FIC decoder

You can also dump the first three raw FIC OFDM softbit symbols before frame assembly:

```sh
DAB_SDRPLAY_USE_REAL=1 \
DAB_SYNC_PROBE_CENTER_FREQ_HZ=213360000 \
DAB_SYNC_PROBE_GAIN_MODE=manual \
DAB_SYNC_PROBE_DUMP_FIC_SYMBOLS_PATH=/tmp/fic_symbols.bin \
./build-release/dab_sdrplay_sync_probe
```

Then try `a/b` swap and polarity mappings offline:

```sh
./build/dab_fic_symbol_dump_analyze /tmp/fic_symbols.bin
```

This reports CRC counts for:
- `a,b`
- `-a,b`
- `a,-b`
- `-a,-b`
- `b,a`
- `-b,a`
- `b,-a`
- `-b,-a`

You can also dump the deinterleaved differential carriers for the first 3 FIC symbols:

```sh
DAB_SDRPLAY_USE_REAL=1 \
DAB_SYNC_PROBE_CENTER_FREQ_HZ=213360000 \
DAB_SYNC_PROBE_GAIN_MODE=manual \
DAB_SYNC_PROBE_DUMP_FIC_CARRIERS_PATH=/tmp/fic_carriers.bin \
./build-release/dab_sdrplay_sync_probe
```

Then try QPSK-axis/sign mappings directly from those carriers:

```sh
./build/dab_fic_carrier_dump_analyze /tmp/fic_carriers.bin
```

This tests the common slicer conventions:
- `-re,-im`
- `re,-im`
- `-re,im`
- `re,im`
- `-im,-re`
- `im,-re`
- `-im,re`
- `im,re`

You can also dump the raw 1537 differential carriers before frequency deinterleave:

```sh
DAB_SDRPLAY_USE_REAL=1 \
DAB_SYNC_PROBE_CENTER_FREQ_HZ=213360000 \
DAB_SYNC_PROBE_GAIN_MODE=manual \
DAB_SYNC_PROBE_DUMP_FIC_DIFF_PATH=/tmp/fic_diff.bin \
./build-release/dab_sdrplay_sync_probe
```

Then try carrier-order plus QPSK-mapping variants:

```sh
./build/dab_fic_diff_dump_analyze /tmp/fic_diff.bin
```

This tests:
- identity order with center removed
- reversed identity order
- current frequency-deinterleave order
- reversed frequency-deinterleave order

against the same 8 QPSK sign/axis mappings.



Useful commands:

./build-release/dab_ref_impl_replay /tmp/samples.bin

DAB_SDRPLAY_USE_REAL=1 DAB_SYNC_PROBE_CENTER_FREQ_HZ=213360000 DAB_SYNC_PROBE_GAIN_MODE=agc DAB_SYNC_PROBE_FAST_WINDOW_SAMPLES=128 DAB_SYNC_PROBE_SLOW_WINDOW_SAMPLES=8192 DAB_SYNC_PROBE_SAMPLE_DECIMATION=4 DAB_SYNC_PROBE_ENTER_RATIO_THRESHOLD=0.35 DAB_SYNC_PROBE_EXIT_RATIO_THRESHOLD=0.55 DAB_SYNC_PROBE_BLOCKS=5000 DAB_SYNC_PROBE_DUMP_REF_SAMPLES_PATH=/tmp/samples.bin ./build-release/dab_sdrplay_sync_probe

gnuplot -e "file='/tmp/prs_fft.gnuplot.dat'; frames=200" /Users/tomaskrajco/src/scratchpad/dab-lib/tools/plot_prs_fft.gnuplot
