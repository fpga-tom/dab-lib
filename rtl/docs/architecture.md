# RTL Architecture

## Pipeline

```text
SDR I/Q stream
  -> iq_ingress
  -> dc_remove_gain
  -> sample_history
  -> null_detector
  -> prs_acquire_track
  -> cfo_corrector
  -> symbol_buffer
  -> fft_2048
  -> active_carrier_mapper
  -> prs_channel_estimator
  -> equalizer
  -> diff_demod
  -> frequency_deinterleaver
  -> qpsk_softbits
  -> tframe_assembler
  -> fic_softbits / msc_softbits DMA or buffer readout
```

## Module Responsibilities

### `iq_ingress`

Accepts signed I/Q samples, assigns a monotonically increasing sample index, and
writes samples into a stream FIFO or circular history RAM.

Initial sample format:

```text
i: signed 16-bit
q: signed 16-bit
sample_index: unsigned 64-bit
```

### `dc_remove_gain`

Optional stage for slow DC estimate subtraction and coarse gain normalization.
This should be bypassable while matching C golden vectors.

### `sample_history`

Stores enough samples to cover PRS search and all 75 data symbols after a locked
PRS. The C tool defaults to a 262144-sample history, which is slightly more than
one 196608-sample Mode I transmission frame.

### `null_detector`

Computes short and long average sample power and emits candidate null-symbol
events:

```text
sample_index
length
min_ratio
```

Match the C detector behavior first: fast/slow power windows, sample decimation,
enter threshold, exit threshold, and min/max null length.

### `prs_acquire_track`

Turns null candidates or predicted frame positions into PRS search windows.
Outputs:

```text
prs_fft_start_sample
prs_score
coarse_freq_error_hz or fixed-point equivalent
carrier_offset_bins
fine_timing_error_samples
locked
```

The first version may accept timing/CFO from testbench control registers so that
OFDM stages can be verified before the full sync loop is complete.

### `cfo_corrector`

Applies an NCO-driven complex rotation before FFT. It should support bypass and
externally supplied phase increment for vector-driven tests.

### `fft_2048`

Wraps a 2048-point complex FFT. Keep the wrapper generic so simulation can use a
simple behavioral model while FPGA builds can use vendor IP.

Required wrapper metadata:

```text
symbol_start_sample
block_exponent or scale schedule
overflow flag
```

### `active_carrier_mapper`

Maps FFT bins into the DAB Mode I active-carrier order used by the C code:

```text
active[0..767]   <- fft[1280..2047]
active[768..1536] <- fft[0..768]
```

Carrier 768 is the DC/null carrier and must be forced to zero for data paths.

### `prs_channel_estimator`

Uses the Mode I PRS reference table to compute per-carrier correction
coefficients. Store one correction vector per locked frame.

### `equalizer`

Applies the stored PRS correction coefficients to each data symbol carrier.

### `diff_demod`

Stores the previous active-carrier vector and computes differential QPSK:

```text
diff = current * conj(previous)
```

The first data symbol after PRS establishes the previous-symbol state. Softbits
are only valid once a previous data symbol exists.

### `frequency_deinterleaver`

Implements the Mode I carrier permutation from `dab_ofdm_build_deinterleave_map`
in the C code. Output count is 1536 complex data carriers.

Source:

```text
rtl/src/ofdm/frequency_deinterleaver.sv
```

The first implementation is a single-symbol buffered permutation stage. It
loads all 1537 active carriers in active-carrier order, including the DC/null
carrier, then emits 1536 complex carriers in C-compatible deinterleaved order.
The DC/null carrier is excluded by the generated map.

The output stream includes both the deinterleaved carrier index and the source
active-carrier index to simplify golden-vector debugging.

### `qpsk_softbits`

Converts deinterleaved differential QPSK carriers into signed soft bits:

```text
soft_a[0..1535]
soft_b[0..1535]
```

Initial target format is signed 8-bit to match the C `int8_t` softbit buffers.

Source:

```text
rtl/src/ofdm/qpsk_softbits.sv
```

The first implementation is a one-carrier-in, one-softbit-pair-out streaming
stage. It computes an integer magnitude and applies the same negative normalized
decision convention as the C receiver:

```text
soft = trunc(component * -127.5 / magnitude)
```

The output preserves carrier index, symbol start metadata, and the symbol-done
marker from the input stream.

### `tframe_assembler`

Packs 75 demodulated symbols per transmission frame:

```text
symbols 0..2   -> FIC, 3 * 3072 softbits
symbols 3..74  -> MSC, 72 * 3072 softbits
```

The output format should be directly comparable with:

```text
DAB_TFRAME_FIC_SOFTBITS
DAB_TFRAME_MSC_SOFTBITS
```

from the C receiver.

## Initial Bring-Up Order

1. `frequency_deinterleaver`
2. `qpsk_softbits`
3. `tframe_assembler`
4. `active_carrier_mapper`
5. `diff_demod`
6. `equalizer`
7. `fft_2048` wrapper
8. `cfo_corrector`
9. `null_detector`
10. `prs_acquire_track`

This order lets deterministic stages pass golden-vector tests before the harder
sync loop is introduced.

## Implemented Blocks

### `tframe_assembler`

Source:

```text
rtl/src/ofdm/tframe_assembler.sv
```

The implemented assembler accepts one signed 8-bit softbit per cycle using a
ready/valid interface. It expects the upstream softbit generator to provide
exactly 3072 softbits per OFDM data symbol in the C-compatible order:

```text
soft_a[0..1535], then soft_b[0..1535]
```

It emits the same softbit value with routing metadata:

```text
out_region = FIC for symbols 0..2, MSC for symbols 3..74
out_addr   = byte address inside the selected FIC or MSC frame buffer
```

The final byte of symbol 74 asserts `out_frame_done`. The downstream integration
can use this stream directly as a DMA/write-port source for FIC and MSC softbit
memories.

Verilator test:

```text
rtl/scripts/run_tframe_assembler_verilator.sh
```

### `frequency_deinterleaver`

Verilator test:

```text
rtl/scripts/run_frequency_deinterleaver_verilator.sh
```

### `qpsk_softbits`

Verilator test:

```text
rtl/scripts/run_qpsk_softbits_verilator.sh
```
