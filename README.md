# dab-lib

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

## Tests

```sh
./build/dab_tests
ctest --test-dir build --output-on-failure
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
- frontend IQ conversion to planar float
- frontend DC removal
- rolling null-symbol detector

Next:
- phase-reference detection after null
- coarse symbol timing
- coarse frequency offset estimation
