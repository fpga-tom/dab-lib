# DAB SDR RTL Prototype

This directory contains the Verilog/SystemVerilog design work for a hardware
version of the DAB SDR receiver. The first target is not a full standalone DAB
radio in RTL. The initial goal is to reproduce the high-rate OFDM front end and
emit the same FIC/MSC softbit frames that the C receiver already consumes.

## Boundary

Initial RTL boundary:

```text
int16 I/Q samples
  -> RTL synchronization and OFDM demodulation
  -> signed 8-bit FIC/MSC softbit frame buffers
  -> existing C FIC/MSC/DAB+/AAC/audio path
```

Keep these stages in software for the first prototype:

- FIC FIG database parsing
- MSC service/subchannel routing policy
- DAB+ superframe interpretation beyond softbit/subchannel decode experiments
- AAC decode
- audio playback

Move these stages to RTL first:

- sample ingress and block/history buffering
- null detection
- PRS acquire/track
- CFO correction
- 2048-point OFDM FFT wrapper
- active-carrier extraction
- PRS channel estimation
- equalization
- differential QPSK demodulation
- frequency deinterleaving
- softbit generation
- transmission-frame assembly

## Layout

```text
rtl/
  docs/
    architecture.md
    fixed_point.md
  src/
    common/       shared stream, FIFO, RAM, arithmetic helpers
    frontend/     sample ingress, DC removal, gain/power measurement
    sync/         null detector, PRS search, timing/CFO tracking
    ofdm/         FFT wrapper, carrier mapping, equalizer, demod, softbits
    fic/          optional later RTL FIC channel decoder experiments
    msc/          optional later RTL MSC channel decoder experiments
    axi/          control/status and DMA-style integration wrappers
  sim/
    tb/           testbenches
    vectors/      golden vectors exported from the C receiver
  scripts/        vector generation and simulation helpers
```

## Interface Style

Use ready/valid streams for data movement:

```text
valid
ready
data
last or symbol_done/frame_done where the packet boundary matters
```

Use control/status registers for slow-changing parameters:

- center frequency metadata
- sample rate metadata
- enable/reset bits
- sync thresholds
- current lock state
- PRS score/CFO/timing telemetry

## Verification Strategy

Use the existing C implementation as the golden model. Export vectors at stable
handoff points and compare RTL output stage by stage:

- raw I/Q blocks
- null events
- PRS FFT bins
- active carriers
- equalized carriers
- differential carriers
- deinterleaved carriers
- soft bits
- complete FIC/MSC softbit frames

The first acceptance milestone is bit-compatible signed 8-bit FIC/MSC softbit
frames for a known capture after timing and CFO are provided or replayed.

## Current Tests

Run the implemented `iq_ingress` Verilator test with:

```sh
rtl/scripts/run_iq_ingress_verilator.sh
```

Run the implemented `sample_history` Verilator test with:

```sh
rtl/scripts/run_sample_history_verilator.sh
```

Run the implemented `symbol_buffer` Verilator test with:

```sh
rtl/scripts/run_symbol_buffer_verilator.sh
```

Run the implemented `cfo_corrector` Verilator test with:

```sh
rtl/scripts/run_cfo_corrector_verilator.sh
```

Run the integrated pipeline-top smoke test with:

```sh
rtl/scripts/run_dab_pipeline_top_verilator.sh
```

Run the file-driven pipeline testbench with the C++ FFT shim and data-symbol
scheduler with:

```sh
DAB_TB_IQ_PATH=/tmp/dab_iq_f32.bin \
DAB_TB_SOFTBITS_PATH=/tmp/dab_softbits_i8.bin \
rtl/scripts/run_dab_pipeline_file_verilator.sh
```

Pass an optional positive integer to decode multiple complete transmission
frames from the same IQ capture:

```sh
DAB_TB_IQ_PATH=/tmp/dab_iq_f32.bin \
DAB_TB_SOFTBITS_PATH=/tmp/dab_softbits_i8.bin \
rtl/scripts/run_dab_pipeline_file_verilator.sh 2
```

The input file is interleaved `float32` IQ (`I0,Q0,I1,Q1,...`) as written by
`DAB_SYNC_PROBE_DUMP_REF_SAMPLES_PATH`. The output file is raw signed `int8`
softbits in `tframe_assembler` order: all FIC softbits first, then MSC
softbits. When multiple frames are requested, the output appends complete
`230400`-softbit frames back-to-back.

Decode and print all FIC prefixes from a raw softbit file with:

```sh
./build-release/dab_fic_softbits_decode /tmp/dab_softbits_i8.bin
```

The decoder walks each complete `230400`-softbit tframe and also decodes a
trailing partial frame when at least one full `9216`-softbit FIC prefix is
present.

Run the implemented `tframe_assembler` Verilator test with:

```sh
rtl/scripts/run_tframe_assembler_verilator.sh
```

Run the implemented `frequency_deinterleaver` Verilator test with:

```sh
rtl/scripts/run_frequency_deinterleaver_verilator.sh
```

Run the implemented `null_detector` Verilator test with:

```sh
rtl/scripts/run_null_detector_verilator.sh
```

Run the implemented `prs_acquire` Verilator test with:

```sh
rtl/scripts/run_prs_acquire_verilator.sh
```

Run the implemented `prs_score_search` Verilator test with:

```sh
rtl/scripts/run_prs_score_search_verilator.sh
```

Run the implemented `prs_acquire_track` Verilator test with:

```sh
rtl/scripts/run_prs_acquire_track_verilator.sh
```

Run the implemented `cp_metric_engine` Verilator test with:

```sh
rtl/scripts/run_cp_metric_engine_verilator.sh
```

Run the implemented `carrier_mapper` Verilator test with:

```sh
rtl/scripts/run_carrier_mapper_verilator.sh
```

Run the implemented `fft_wrapper` Verilator test with:

```sh
rtl/scripts/run_fft_wrapper_verilator.sh
```

Run the implemented `diff_demod` Verilator test with:

```sh
rtl/scripts/run_diff_demod_verilator.sh
```

Run the implemented `equalizer` Verilator test with:

```sh
rtl/scripts/run_equalizer_verilator.sh
```

Run the implemented `prs_channel_estimator` Verilator test with:

```sh
rtl/scripts/run_prs_channel_estimator_verilator.sh
```

Run the implemented `prs_pilot_refiner` Verilator test with:

```sh
rtl/scripts/run_prs_pilot_refiner_verilator.sh
```

Run the implemented `qpsk_softbits` Verilator test with:

```sh
rtl/scripts/run_qpsk_softbits_verilator.sh
```
