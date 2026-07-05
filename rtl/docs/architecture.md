# RTL Architecture

## Pipeline

```text
SDR I/Q stream
  -> iq_ingress
  -> dc_remove_gain
  -> sample_history
  -> null_detector
  -> prs_acquire_track
  -> cp_stream_metric / prs_stream_search
  -> symbol_buffer
  -> cfo_corrector
  -> fft_2048
  -> active_carrier_mapper
  -> prs_pilot_refiner
  -> prs_channel_estimator
  -> equalizer
  -> diff_demod
  -> frequency_deinterleaver
  -> qpsk_softbits
  -> tframe_assembler
  -> fic_softbits / msc_softbits DMA or buffer readout
```

## Module Responsibilities

### `dab_pipeline_top`

Wires the current RTL receiver blocks into one compileable integration boundary.
The top owns:

```text
iq_ingress -> sample_history/null_detector
sample_history read arbitration for symbol extraction
streaming CP metric/search for PRS coarse timing
PRS acquire/track control
symbol_buffer -> cfo_corrector -> fft_2048 -> active_carrier_mapper
PRS pilot refinement and channel coefficient update
data equalize -> diff_demod -> deinterleave -> softbits -> tframe_assembler
```

Source:

```text
rtl/src/top/dab_pipeline_top.sv
```

The FFT math remains an external core interface. By default, data-symbol
scheduling is internal: `data_symbol_scheduler` observes accepted sync events
and requests each of the 75 data symbols only when that symbol's 2048-sample
FFT window is present in `sample_history`. It can move directly from one frame
to the next after the current frame's symbol commands have been accepted; it
does not wait for `tframe_assembler` to emit the final softbit. The legacy external
`data_symbol_cmd_*` path remains available by setting
`ENABLE_INTERNAL_DATA_SCHEDULER=0`. The sync path internally schedules PRS
symbols through `prs_acquire_track`. The top includes a softbit-pair adapter
that stores QPSK `soft_b` values so `tframe_assembler` still receives the
C-compatible `soft_a[0..1535]` then `soft_b[0..1535]` order.

The Verilator file replay defaults to four fabric cycles per accepted SDR
sample when the internal scheduler and streaming CP search are enabled. This
models the FPGA clock running faster than the 2.048 Msps sample strobe. A
single-cycle sample strobe is intentionally not enough for the current
one-softbit-per-cycle output interface because each 2552-sample DAB data symbol
produces 3072 softbits.

By default, PRS CP scoring is independent of `sample_history`:
`cp_stream_metric` computes a rolling cyclic-prefix score as samples arrive, and
`prs_stream_search` selects the best live metric inside each acquire/tracking
window. The older history-backed `prs_score_search` plus `cp_metric_engine` path
is still available by setting `ENABLE_STREAMING_CP_SEARCH=0`.

### `iq_ingress`

Accepts signed I/Q samples, assigns a monotonically increasing sample index, and
writes samples into a stream FIFO or circular history RAM.

Initial sample format:

```text
i: signed 16-bit
q: signed 16-bit
sample_index: unsigned 64-bit
```

Source:

```text
rtl/src/frontend/iq_ingress.sv
```

The first implementation is a ready/valid tagging stage. It accepts one complex
sample per beat, passes the signed I/Q values through unchanged, and emits the
current 64-bit sample index with the same beat. The internal index increments
only when both input and output handshake.

Control can seed the absolute index with:

```text
set_sample_index_valid
set_sample_index_value
```

While the set-index control is asserted, the sample stream is stalled and no
output beat is emitted. `clear` resets the index to zero and suppresses
handshakes for that cycle.

### `dc_remove_gain`

Optional stage for slow DC estimate subtraction and coarse gain normalization.
This should be bypassable while matching C golden vectors.

### `sample_history`

Stores enough samples for in-flight PRS/data symbol extraction. CP search no
longer requires future IQ samples to be retained in this buffer when
`ENABLE_STREAMING_CP_SEARCH=1`. Data symbols are also not held until a full
transmission frame is available. The C file replay now defaults to a
32768-sample history. With the default file replay pacing, this is enough to
retain the symbol windows needed by the internally scheduled data path.

Source:

```text
rtl/src/frontend/sample_history.sv
```

The first implementation is a power-of-two circular sample RAM with one write
stream and one contiguous read-window stream. It stores signed I/Q samples keyed
by their absolute 64-bit sample index:

```text
write: in_i, in_q, in_sample_index
read request: read_req_start_sample, read_req_length
read output: rd_out_i, rd_out_q, rd_out_sample_index, rd_out_first, rd_out_last
```

The block tracks the contiguous valid range:

```text
oldest_sample_index <= readable sample < next_sample_index
stored_samples <= DEPTH_SAMPLES
```

If input sample indices are not contiguous, the valid range is restarted at the
new sample index. A read request outside the valid range, or with zero length,
returns one `rd_out_error` beat with `rd_out_first` and `rd_out_last` asserted.

### `null_detector`

Computes short and long average sample power and emits candidate null-symbol
events:

```text
sample_index
length
avg_power
min_ratio
```

Match the C detector behavior first: fast/slow power windows, sample decimation,
enter threshold, exit threshold, and min/max null length.

Source:

```text
rtl/src/sync/null_detector.sv
```

The first implementation accepts one signed I/Q sample per cycle with an
explicit 64-bit sample index. It keeps separate fast and slow moving sums over
parameterized ring buffers, compares the normalized fast/slow power ratio by
cross-multiplication, and emits a ready/valid null event when a low-power run
exits and passes the configured checks:

```text
min_null_samples
max_null_samples
startup_ignore_samples
expected_interval_samples +/- interval_tolerance_samples
max_merge_gap_samples
```

The detector reports `avg_power` as an integer average over the accepted run and
`min_ratio` as unsigned Q2.22. Detection decisions do not use division. The
current RTL processes every sample; `sample_decimation` from the C detector is
left for the later ingress/control wrapper.

### `prs_acquire`

Converts accepted null-symbol events into PRS timing candidates.

Source:

```text
rtl/src/sync/prs_acquire.sv
```

The first implementation mirrors `dab_prs_candidate_from_null` from the C
receiver:

```text
prs_symbol_start_sample = null_start_sample + null_length
prs_fft_start_sample = prs_symbol_start_sample + guard_samples
timing_search_start_offset = acquire_window_samples / 2
timing_search_length = acquire_window_samples
```

It also emits the derived timing search bounds:

```text
search_start_sample = prs_fft_start_sample - timing_search_start_offset
search_end_sample = search_start_sample + timing_search_length
required_end_sample = search_end_sample + fft_samples
```

This block does not score PRS candidates. In the default pipeline,
`prs_stream_search` consumes the candidate window and selects the best live
metric result from `cp_stream_metric`. The older `prs_score_search` stage can
still be used with `cp_metric_engine` by disabling streaming CP search.

### `cp_stream_metric`

Continuously computes the cyclic-prefix metric as IQ samples arrive.

Source:

```text
rtl/src/sync/cp_stream_metric.sv
```

The block delays the live sample stream by `fft_samples`, forms the per-sample
prefix/tail product, and keeps rolling `guard_samples` sums:

```text
corr_re += ai * bi + aq * bq
corr_im += aq * bi - ai * bq
score = |corr|^2 / (energy_prefix * energy_tail)
```

Once warmed up, it emits one metric per accepted sample:

```text
out_fft_start_sample = tail_sample_index - fft_samples + 1
out_score
out_coarse_freq_error_hz
```

This replaces the previous request/read/correlate loop for the default sync
path and reduces the CP search memory requirement to a delay line plus a
`guard_samples` rolling-product buffer.

### `prs_stream_search`

Selects the best streaming CP metric inside a PRS search window.

Source:

```text
rtl/src/sync/prs_stream_search.sv
```

It accepts the same search metadata emitted by `prs_acquire_track` and returns
the same score packet shape as `prs_score_search`, so the downstream
acquire/track and PRS pilot-refinement path is unchanged.

### `prs_score_search`

Legacy history-backed timing-search control loop for a PRS candidate.

Runs the timing-search control loop for a PRS candidate.

Source:

```text
rtl/src/sync/prs_score_search.sv
```

The implementation mirrors the search loop in `dab_prs_score_search_i16`
without embedding the cyclic-prefix correlation datapath. For each candidate it
issues one metric request per possible FFT start:

```text
metric_req_fft_start_sample = search_start_sample .. search_end_sample - 1
```

The `cp_metric_engine` returns:

```text
metric_resp_ok
metric_resp_fft_start_sample
metric_resp_score
metric_resp_coarse_freq_error_hz
```

`prs_score_search` counts all responses, ignores invalid metric responses,
keeps the highest score, and emits a C-style result:

```text
found
prs_peak_sample
prs_symbol_start_sample = prs_fft_start_sample - guard_samples
prs_fft_start_sample
prs_score
coarse_freq_error_hz
```

The current score format is unsigned Q2.30. Coarse frequency error is currently
a signed integer Hz field at the controller boundary.

### `cp_metric_engine`

Legacy history-backed cyclic-prefix metric engine.

Computes the cyclic-prefix metric for each PRS FFT-start candidate.

Source:

```text
rtl/src/sync/cp_metric_engine.sv
```

For each request from `prs_score_search`, the engine reads one contiguous
history window:

```text
read_start  = fft_start_sample - guard_samples
read_length = fft_samples + guard_samples
```

It stores the first `guard_samples` prefix samples locally, skips the useful
symbol body, then correlates those prefix samples against the final
`guard_samples` useful-symbol tail:

```text
corr_re += ai * bi + aq * bq
corr_im += aq * bi - ai * bq
score = |corr|^2 / (energy_prefix * energy_tail)
```

The response matches the `prs_score_search` metric response interface:

```text
metric_resp_ok
metric_resp_fft_start_sample
metric_resp_score
metric_resp_coarse_freq_error_hz
```

Invalid FFT starts, stale/missing history windows, malformed read streams, and
zero-energy windows return `metric_resp_ok = 0`. The score calculation is exact
integer arithmetic in unsigned Q2.30 for the current accumulator widths. The
coarse CFO field is signed integer Hz using a small-angle phase estimate with a
`+/-500 Hz` clamp; replace this with an atan2/CORDIC path if wider-range CFO
accuracy is needed.

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

Source:

```text
rtl/src/sync/prs_acquire_track.sv
```

The first implementation is the lock controller around `prs_score_search` and
`prs_pilot_refiner`. While unlocked, null events produce the same wide acquire
window as `prs_acquire`. Once locked, either a null trigger or an explicit
prediction trigger produces a narrow tracking window around the predicted next
PRS FFT start.

The block accepts the CP score result, emits a PRS symbol request for the
FFT/pilot-refinement path, then combines CP score/CFO with pilot score, carrier
offset, and Q12.12 fine timing. Successful pilot refinements update the next
frame prediction as:

```text
next_prs_fft_start = current_prs_fft_start + 196608 + round(fine_timing_q12)
```

Lock is asserted after `LOCK_CONFIRM_COUNT` consecutive successful refinements
and dropped after `UNLOCK_THRESHOLD` consecutive misses.

### `symbol_buffer`

Reads one FFT-sized contiguous symbol window from `sample_history` after sync
selects a symbol start sample. It does not allocate a second full-symbol RAM;
the block is a request/stream adapter that preserves symbol metadata for the
FFT-facing path.

Source:

```text
rtl/src/ofdm/symbol_buffer.sv
```

The first implementation requests exactly 2048 samples from history and streams
them with:

```text
out_symbol_start_sample
out_sample_index
out_sample_offset = 0..2047
out_symbol_done on sample 2047
```

Stale or unavailable history, unexpected sample indices, bad first/last markers,
or truncated windows produce a single `out_error` beat with
`out_symbol_done = 1`. If the history stream has already started, the block
drains the remaining response before accepting another symbol request.

### `cfo_corrector`

Applies an NCO-driven complex rotation before FFT. It should support bypass and
externally supplied phase increment for vector-driven tests.

Source:

```text
rtl/src/ofdm/cfo_corrector.sv
```

The first implementation is a streaming CORDIC rotator. It consumes raw 16-bit
symbol samples from `symbol_buffer`, uses a 32-bit phase accumulator with
externally loaded seed and phase increment, and emits corrected 18-bit I/Q plus
the original sample index, symbol start, sample offset, done, error, and phase
metadata.

The `cfg_bypass` path preserves raw I/Q while still exercising the same
ready/valid and metadata latency. Error beats are passed through with zero I/Q
and do not advance the NCO phase.

### `fft_2048`

Wraps a 2048-point complex FFT. Keep the wrapper generic so simulation can use a
simple behavioral model while FPGA builds can use vendor IP.

Source:

```text
rtl/src/ofdm/fft_wrapper.sv
rtl/src/ofdm/fft_radix2_core.sv
rtl/src/ofdm/altera_fft_avalon_core.sv
rtl/src/top/dab_pipeline_top_custom_fft.sv
rtl/src/top/dab_pipeline_top_quartus_fft.sv
```

Required wrapper metadata:

```text
symbol_start_sample
block_exponent or scale schedule
overflow flag
```

The wrapper is a streaming shell around an external FFT core. Input and output
sides can run at the same time, so the wrapper can feed the next 2048-sample
symbol while bins for an earlier symbol are still leaving the core. A small
metadata FIFO preserves the symbol start sample for in-flight FFT frames. The
input side forwards signed complex time-domain samples to the core with
`core_in_last` asserted on sample 2047, then the output side tags the core's
natural-order bin stream with:

```text
out_fft_bin_index = 0..2047
out_symbol_start_sample = sample index captured from input sample 0
out_block_exp = core-reported scale metadata
out_overflow = core-reported overflow metadata
out_symbol_done = bin 2047
```

The wrapper intentionally does not implement FFT math. FPGA builds should bind
the `core_*` ports to the selected FFT IP. Verilator tests use a mock core so
the wrapper's framing, metadata, and backpressure behavior can be verified
without choosing vendor IP yet.

For Intel/Altera Quartus builds, use `dab_pipeline_top_quartus_fft` as the top
entity. It instantiates the normal `dab_pipeline_top`, hides the `fft_core_*`
ports, and connects them to `altera_fft_avalon_core`. The adapter instantiates a
Quartus-generated FFT IP entity named `dab_fft_2048` by default; override the
entity name with the compile macro `DAB_ALTERA_FFT_IP_MODULE` if the generated
IP uses a different module name.

Expected Quartus FFT IP configuration:

```text
transform length: 2048, fixed length
direction: inverse input port enabled; adapter drives it low for forward FFT
streaming interface: Avalon-ST sink/source
output order: natural order
arithmetic: block floating point with source_exp enabled
sink_real/sink_imag width: 18 bits by default
source_real/source_imag width: 24 bits by default
source_exp width: 8 bits by default
sink_error/source_error width: 2 bits by default
```

If the generated IP uses different data, exponent, or error widths, set the
`FFT_IP_INPUT_WIDTH`, `FFT_IP_OUTPUT_WIDTH`, `FFT_IP_EXP_WIDTH`, and
`FFT_IP_ERROR_WIDTH` parameters on `dab_pipeline_top_quartus_fft` to match the
IP. Add the generated `.qip`/`.ip` files to the Quartus project together with
the RTL sources.

For a vendor-independent build, use `dab_pipeline_top_custom_fft` as the top
entity. It instantiates `fft_radix2_core` behind the same `core_*` FFT wrapper
interface. The custom core is an in-place radix-2 decimation-in-time FFT:

```text
load: 2048 samples into bit-reversed memory addresses
compute: 11 stages, 1024 butterflies per stage, 2 cycles per butterfly
output: 2048 natural-order bins from synchronous RAM
```

The core uses one true dual-port complex sample memory, represented as separate
I and Q RAMs, and a stage-step twiddle recurrence instead of a 1024-entry
twiddle ROM. By default it uses 24-bit internal data, 18-bit Q1.16 twiddles, and
scales each butterfly by one bit. The output therefore reports
`core_out_block_exp = 11`, equivalent to an FFT result divided by 2048. This
keeps the datapath bounded without a dynamic block-floating controller. The
core needs roughly 28.7k fabric cycles per symbol, so continuous DAB Mode I
operation needs a fabric clock above about 11.3 cycles per 2.048 Msps input
sample. The core is intended as a portable first hardware FFT; a later version
can pipeline the butterfly or add ping-pong memories if the clock target needs
more margin.

### `dab_zynq_axis_top`

Zynq integration wrapper for `dab_pipeline_top_custom_fft`.

Source:

```text
rtl/src/top/dab_zynq_axis_top.v
```

The wrapper exposes one clock domain intended for a Zynq PS fabric clock such as
`FCLK_CLK0`: AXI-Lite control/status, AXI-Stream IQ input, and AXI-Stream
softbit output. The IQ stream carries one complex sample per beat:

```text
s_axis_iq_tdata[15:0]  = signed I
s_axis_iq_tdata[31:16] = signed Q
```

The softbit stream packs four signed 8-bit softbits per 32-bit AXI beat. The
first softbit is in bits `[7:0]`, and `m_axis_softbit_tlast` is asserted on the
packed word containing the DAB transmission-frame end.

The AXI-Lite block provides run/clear controls, threshold and CFO registers,
sample-index initialization, counters, latest sync metrics, frame count, and
history status. For normal autonomous operation, leave the internal data
scheduler enabled, write the desired thresholds, optionally pulse
`set_sample_index`, then set `CONTROL.run_enable`.

### `active_carrier_mapper`

Maps FFT bins into the DAB Mode I active-carrier order used by the C code:

```text
active[0..767]   <- fft[1280..2047]
active[768..1536] <- fft[0..768]
```

Carrier 768 is the DC/null carrier and must be forced to zero for data paths.

Source:

```text
rtl/src/ofdm/carrier_mapper.sv
```

The first implementation accepts natural-order FFT bins. It stores only the low
active bin range `0..768`, streams high active bins `1280..2047` directly as
`active[0..767]`, then emits the stored low range as `active[768..1536]`. This
avoids a full 2048-bin symbol buffer inside the mapper.

### `prs_channel_estimator`

Uses the Mode I PRS reference table to compute per-carrier correction
coefficients. Store one correction vector per locked frame.

Source:

```text
rtl/src/ofdm/prs_channel_estimator.sv
```

The first implementation accepts the active-carrier PRS stream and emits two
ready/valid streams:

```text
coeff_index, coeff_i, coeff_q
ref_active_index, ref_i, ref_q
```

The coefficient stream is formatted for the equalizer's coefficient write port:

```text
coeff = reference * conj(received_prs) / |received_prs|^2
```

The PRS reference stream emits the ideal Mode I QPSK PRS carriers in active
carrier order. Integration can mux this stream into `diff_demod` to prime the
previous-symbol state exactly like the C receiver does after estimating the
channel from PRS.

The PRS reference is generated from the compact 4x32 phase pattern used by the
C table generator, not from a 1536-entry reference ROM. The first coefficient
calculator uses an exact signed divider for correctness and vector bring-up;
replace that with a reciprocal/divider pipeline before optimizing for timing.

### `prs_pilot_refiner`

Scores the PRS FFT active-carrier stream against the Mode I PRS pilots after
the coarse CP-window search has selected an FFT start.

Source:

```text
rtl/src/sync/prs_pilot_refiner.sv
```

The block mirrors the differential PRS pilot scoring used by
`dab_prs_score_reference_bins_diff_f32`. It evaluates carrier-offset hypotheses
`-4..+4` while streaming the active carriers in C order:

```text
active[0..767]    -> fft[1280..2047]
active[768]       -> fft[0] DC/null
active[769..1536] -> fft[1..768]
```

For each offset hypothesis it keeps only the previous valid received/reference
carrier and accumulates adjacent-bin differential correlation:

```text
sd = rx_previous * conj(rx_current)
pd = prs_previous * conj(prs_current)
corr += sd * conj(pd)
```

The PRS pilot phases are generated from the compact 4x32 phase pattern, so this
stage does not store a 1536-entry pilot ROM. After `in_symbol_done`, it chooses
the highest normalized correlation score and emits:

```text
out_score
out_carrier_offset_bins
out_fine_timing_error_samples
out_used_pairs
```

`out_fine_timing_error_samples` is signed Q12.12 samples. The first hardware
conversion uses the small-angle form:

```text
timing ~= imag(corr) / real(corr) * 2048 / (2*pi)
```

This matches the intended CP-refined tracking range. A later implementation can
replace that divider with an atan2/CORDIC path if wider unlocked-range phase
reporting is needed.

### `equalizer`

Applies the stored PRS correction coefficients to each data symbol carrier.

Source:

```text
rtl/src/ofdm/equalizer.sv
```

The first implementation accepts active carriers in active-carrier order and
applies a complex coefficient from an internal channel table:

```text
out_i = carrier_i * coeff_i - carrier_q * coeff_q
out_q = carrier_i * coeff_q + carrier_q * coeff_i
```

Coefficients use the fixed-point format from `dab_ofdm_pkg`. The PRS channel
estimator will populate this table through the coefficient write port in a later
step. Carrier 768 is forced to zero to match the C receiver's DC/null carrier
handling.

### `diff_demod`

Stores the previous active-carrier vector and computes differential QPSK:

```text
diff = current * conj(previous)
```

The first data symbol after PRS establishes the previous-symbol state. Softbits
are only valid once a previous data symbol exists.

Source:

```text
rtl/src/ofdm/diff_demod.sv
```

The first implementation accepts active carriers in active-carrier order. It
stores one previous active-carrier vector, emits no output while priming that
state, then outputs scaled fixed-point differential carriers for each following
symbol. Carrier 768 is forced to zero to match the C receiver's DC/null carrier
handling.

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
The DC/null carrier is excluded by the generated sequence.

The deinterleaver does not store a 1536-entry map ROM. It generates source
addresses on the fly with the DAB Mode I recurrence:

```text
p_next = (13 * p + 511) mod 2048
```

Only candidates in `256..1792` except `1024` are emitted, with source active
carrier index `p - 256`. The output stream includes both the deinterleaved
carrier index and the source active-carrier index to simplify golden-vector
debugging.

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
8. `prs_channel_estimator`
9. `prs_pilot_refiner`
10. `null_detector`
11. `prs_acquire`
12. `prs_score_search`
13. `iq_ingress`
14. `sample_history`
15. `cp_metric_engine`
16. `cp_stream_metric`
17. `prs_stream_search`
18. `symbol_buffer`
19. `cfo_corrector`
20. `prs_acquire_track`

This order lets deterministic stages pass golden-vector tests before the harder
sync loop is introduced.

## Implemented Blocks

### `dab_pipeline_top`

Source:

```text
rtl/src/top/dab_pipeline_top.sv
```

The implemented top is a first integration boundary for Verilator and later FPGA
top-level work. It uses a single-read history arbiter, keeps the FFT core
external, schedules data symbols internally by default, and routes PRS symbols
through pilot refinement/channel estimation while data symbols continue into
the transmission-frame assembler. External data-symbol start commands are still
available when `ENABLE_INTERNAL_DATA_SCHEDULER=0`.

Verilator test:

```text
rtl/scripts/run_dab_pipeline_top_verilator.sh
```

File replay Verilator test with a C++ FFT shim and the default internal
data-symbol scheduler:

```text
rtl/scripts/run_dab_pipeline_file_verilator.sh
```

File replay Verilator test through the custom synthesizable FFT:

```text
rtl/scripts/run_dab_pipeline_file_custom_fft_verilator.sh
```

The custom FFT file replay defaults to `DAB_TB_SAMPLE_PERIOD_CYCLES=16` so the
iterative RTL FFT has enough cycles to load, compute, and emit each data symbol.

### `iq_ingress`

Source:

```text
rtl/src/frontend/iq_ingress.sv
```

The implemented ingress block provides the front-end stream metadata boundary.
It accepts signed 16-bit I/Q by default, forwards the samples unchanged, and
attaches a monotonically increasing 64-bit sample index. The counter can be
seeded with a control beat before starting or resuming a capture stream.

Verilator test:

```text
rtl/scripts/run_iq_ingress_verilator.sh
```

### `sample_history`

Source:

```text
rtl/src/frontend/sample_history.sv
```

The implemented history block stores the ingress stream in a circular RAM and
serves contiguous sample windows by absolute sample index. It is intended to
feed later CP metric and FFT symbol-buffer readers.

Verilator test:

```text
rtl/scripts/run_sample_history_verilator.sh
```

### `symbol_buffer`

Source:

```text
rtl/src/ofdm/symbol_buffer.sv
```

The implemented symbol buffer accepts an absolute FFT symbol start sample,
requests a contiguous window from `sample_history`, and streams raw I/Q samples
with symbol start, sample index, offset, done, and error metadata. It is intended
to feed the later CFO-correction/FFT input path without duplicating the history
RAM.

Verilator test:

```text
rtl/scripts/run_symbol_buffer_verilator.sh
```

### `cfo_corrector`

Source:

```text
rtl/src/ofdm/cfo_corrector.sv
```

The implemented corrector applies an NCO/CORDIC complex rotation to each valid
symbol sample, supports deterministic phase seed/load and per-sample phase
increment registers, and preserves the symbol metadata needed by the FFT input
path. Bypass mode forwards I/Q unchanged through the same pipeline latency.

Verilator test:

```text
rtl/scripts/run_cfo_corrector_verilator.sh
```

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

### `null_detector`

Verilator test:

```text
rtl/scripts/run_null_detector_verilator.sh
```

### `prs_acquire`

Verilator test:

```text
rtl/scripts/run_prs_acquire_verilator.sh
```

### `prs_score_search`

Legacy path used when `ENABLE_STREAMING_CP_SEARCH=0`.

Verilator test:

```text
rtl/scripts/run_prs_score_search_verilator.sh
```

### `cp_stream_metric`

Source:

```text
rtl/src/sync/cp_stream_metric.sv
```

Continuously scores cyclic-prefix correlation from the live sample stream.

### `prs_stream_search`

Source:

```text
rtl/src/sync/prs_stream_search.sv
```

Selects the best continuous CP metric inside each acquire/tracking window.

### `prs_acquire_track`

Source:

```text
rtl/src/sync/prs_acquire_track.sv
```

The implemented track controller generates acquire/tracking search windows,
requests PRS pilot refinement after a passing CP score, updates lock state, and
reports coarse CFO plus pilot carrier/fine-timing metadata.

Verilator test:

```text
rtl/scripts/run_prs_acquire_track_verilator.sh
```

### `cp_metric_engine`

Source:

```text
rtl/src/sync/cp_metric_engine.sv
```

Legacy path used when `ENABLE_STREAMING_CP_SEARCH=0`. The implemented engine
consumes `prs_score_search` metric requests, reads
sample windows from `sample_history`, and returns CP correlation score plus
coarse CFO metadata.

Verilator test:

```text
rtl/scripts/run_cp_metric_engine_verilator.sh
```

### `carrier_mapper`

Verilator test:

```text
rtl/scripts/run_carrier_mapper_verilator.sh
```

### `diff_demod`

Verilator test:

```text
rtl/scripts/run_diff_demod_verilator.sh
```

### `equalizer`

Verilator test:

```text
rtl/scripts/run_equalizer_verilator.sh
```

### `prs_channel_estimator`

Verilator test:

```text
rtl/scripts/run_prs_channel_estimator_verilator.sh
```

### `prs_pilot_refiner`

Verilator test:

```text
rtl/scripts/run_prs_pilot_refiner_verilator.sh
```

### `fft_wrapper`

Verilator test:

```text
rtl/scripts/run_fft_wrapper_verilator.sh
rtl/scripts/run_fft_radix2_core_verilator.sh
```

Custom FFT top lint:

```text
rtl/scripts/run_dab_pipeline_top_custom_fft_lint.sh
```

Zynq AXI wrapper lint:

```text
rtl/scripts/run_dab_zynq_axis_top_lint.sh
```

Quartus FFT top lint:

```text
rtl/scripts/run_dab_pipeline_top_quartus_fft_lint.sh
```

### `qpsk_softbits`

Verilator test:

```text
rtl/scripts/run_qpsk_softbits_verilator.sh
```
