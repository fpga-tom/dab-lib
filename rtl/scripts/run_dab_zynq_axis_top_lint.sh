#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
CHANNEL_EQUALIZER="${DAB_TB_ENABLE_CHANNEL_EQUALIZER:-0}"
QPSK_RAW_SHIFT="${DAB_TB_QPSK_RAW_SHIFT:-9}"
INTERNAL_CFO_CONTROL="${DAB_TB_ENABLE_INTERNAL_CFO_CONTROL:-0}"
CFO_CORRECTOR="${DAB_TB_ENABLE_CFO_CORRECTOR:-${INTERNAL_CFO_CONTROL}}"

verilator \
  -Wall \
  --lint-only \
  -GSAMPLE_HISTORY_DEPTH=4096 \
  -GENABLE_STREAMING_CP_SEARCH=1 \
  -GNULL_FAST_WINDOW_SAMPLES=4 \
  -GNULL_SLOW_WINDOW_SAMPLES=16 \
  -GNULL_MIN_SAMPLES=4 \
  -GENABLE_CHANNEL_EQUALIZER="${CHANNEL_EQUALIZER}" \
  -GQPSK_RAW_SHIFT="${QPSK_RAW_SHIFT}" \
  -GENABLE_CFO_CORRECTOR="${CFO_CORRECTOR}" \
  -GENABLE_INTERNAL_CFO_CONTROL="${INTERNAL_CFO_CONTROL}" \
  "${ROOT_DIR}/rtl/src/common/dab_ofdm_pkg.sv" \
  "${ROOT_DIR}/rtl/src/common/dab_tframe_pkg.sv" \
  "${ROOT_DIR}/rtl/src/frontend/iq_ingress.sv" \
  "${ROOT_DIR}/rtl/src/frontend/sample_history.sv" \
  "${ROOT_DIR}/rtl/src/sync/null_detector.sv" \
  "${ROOT_DIR}/rtl/src/sync/cp_stream_metric.sv" \
  "${ROOT_DIR}/rtl/src/sync/prs_stream_search.sv" \
  "${ROOT_DIR}/rtl/src/sync/prs_score_search.sv" \
  "${ROOT_DIR}/rtl/src/sync/cp_metric_engine.sv" \
  "${ROOT_DIR}/rtl/src/sync/prs_pilot_refiner.sv" \
  "${ROOT_DIR}/rtl/src/sync/prs_acquire_track.sv" \
  "${ROOT_DIR}/rtl/src/ofdm/symbol_buffer.sv" \
  "${ROOT_DIR}/rtl/src/ofdm/cfo_corrector.sv" \
  "${ROOT_DIR}/rtl/src/ofdm/fft_wrapper.sv" \
  "${ROOT_DIR}/rtl/src/ofdm/fft_radix2_core.sv" \
  "${ROOT_DIR}/rtl/src/ofdm/carrier_mapper.sv" \
  "${ROOT_DIR}/rtl/src/ofdm/prs_channel_estimator.sv" \
  "${ROOT_DIR}/rtl/src/ofdm/equalizer.sv" \
  "${ROOT_DIR}/rtl/src/ofdm/diff_demod.sv" \
  "${ROOT_DIR}/rtl/src/ofdm/frequency_deinterleaver.sv" \
  "${ROOT_DIR}/rtl/src/ofdm/qpsk_softbits.sv" \
  "${ROOT_DIR}/rtl/src/ofdm/tframe_assembler.sv" \
  "${ROOT_DIR}/rtl/src/top/data_symbol_scheduler.sv" \
  "${ROOT_DIR}/rtl/src/top/dab_pipeline_top.sv" \
  "${ROOT_DIR}/rtl/src/top/dab_pipeline_top_custom_fft.sv" \
  "${ROOT_DIR}/rtl/src/top/dab_zynq_axis_top.v" \
  --top-module dab_zynq_axis_top
