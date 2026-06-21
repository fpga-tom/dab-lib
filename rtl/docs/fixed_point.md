# Fixed-Point Formats

This document defines the starting fixed-point formats for the RTL prototype.
They are intentionally conservative and can be tightened after vector-based
range measurements.

Notation:

```text
Qm.n = signed fixed point with m integer bits including sign and n fractional bits
```

## External Formats

| Signal | Format | Notes |
| --- | --- | --- |
| Input I/Q | signed 16-bit integer | Same logical format as HAL `int16_t` I/Q blocks |
| Output soft bits | signed 8-bit integer | Matches C `int8_t` softbit buffers |
| Sample index | unsigned 64-bit integer | Matches C sample counters |
| Symbol/frame counters | unsigned 16-bit or wider | 75 symbols per Mode I frame |

## Stage Formats

| Stage | Suggested Format | Notes |
| --- | --- | --- |
| Power `I*I + Q*Q` | unsigned 34-bit | 16-bit squared terms summed |
| Fast/slow average power | unsigned 40-bit accumulator | Leave headroom for long windows |
| Null power ratio | Q2.22 or compare by cross-multiply | Prefer cross-multiply for thresholds |
| NCO phase | unsigned 32-bit phase accumulator | Full cycle wraps at 2^32 |
| NCO sin/cos | Q1.17 or Q1.15 | Choose by DSP multiplier budget |
| CFO-corrected I/Q | signed 18-bit | Keep 1-2 guard bits after mixer |
| FFT input | signed 18-bit complex | Wrapper forwards CFO-corrected samples to FFT IP |
| FFT internal | vendor scale schedule or block floating | Export scale exponent |
| FFT output | signed 24-bit complex | Initial generic target |
| Active carriers | signed 24-bit complex | Same as FFT output after mapping |
| Channel correction | signed 24-bit Q2.22 complex | Derived from PRS reference / received carrier |
| Equalized carriers | signed 24-bit complex | Saturate or round after complex multiply |
| Differential carriers | signed 24-bit complex | `current * conj(previous)`, scaled down |
| Deinterleaved carriers | signed 24-bit complex | Permuted differential carriers |
| Softbits | signed 8-bit | Saturated from carrier I/Q confidence |

## Softbit Convention

Use signed softbits with zero as the decision boundary:

```text
positive -> bit confidence for one QPSK half-plane
negative -> opposite half-plane
larger magnitude -> stronger confidence
```

The C code stores each OFDM data symbol as:

```text
soft_a[0..1535]
soft_b[0..1535]
```

and the transmission-frame assembler packs:

```text
dst[0..1535]    = soft_a
dst[1536..3071] = soft_b
```

The RTL output should preserve this order exactly.

## Saturation And Rounding

Use explicit saturation at every width-reducing boundary. Avoid silent wrap on
DSP outputs.

Recommended default:

```text
multiply -> full precision
scale/shift -> round to nearest by adding half LSB before truncation
clip -> signed saturation
```

For early bit-exact comparisons against C floating-point output, keep debug
builds that can expose wider intermediate values before clipping.

## FFT Scaling

The FFT wrapper must report its scale schedule or block exponent. Downstream
stages should treat FFT amplitude as relative, because PRS channel estimation
and softbit clipping can absorb a fixed gain factor.

Initial plan:

```text
simulation FFT: unscaled or known fixed scale
FPGA FFT IP: block floating or configured per-stage scaling
metadata: block_exponent attached to each symbol
```

## Thresholds

Avoid division in sync hardware where possible.

For null detection, compare:

```text
fast_power / slow_power < threshold
```

as:

```text
fast_power * threshold_den < slow_power * threshold_num
```

For PRS score thresholds, keep a Q1.15 or Q2.14 register format until measured
range data suggests otherwise.

## Open Measurement Tasks

Before freezing widths, capture ranges from the C receiver for:

- PRS FFT bin magnitudes
- equalized carrier magnitudes
- differential carrier magnitudes
- softbit distributions for good and marginal signals
- CFO correction mixer output peaks

Those measurements should drive the final synthesis-oriented widths.
