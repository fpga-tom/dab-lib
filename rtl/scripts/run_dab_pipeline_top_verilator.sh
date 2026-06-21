#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD_DIR="${ROOT_DIR}/build/rtl/dab_pipeline_top"

mkdir -p "${BUILD_DIR}"

verilator \
  -Wall \
  -GSAMPLE_HISTORY_DEPTH=4096 \
  -GNULL_FAST_WINDOW_SAMPLES=4 \
  -GNULL_SLOW_WINDOW_SAMPLES=16 \
  -GNULL_MIN_SAMPLES=4 \
  --cc "${ROOT_DIR}/rtl/src/common/dab_ofdm_pkg.sv" \
       "${ROOT_DIR}/rtl/src/common/dab_tframe_pkg.sv" \
       "${ROOT_DIR}/rtl/src/frontend/iq_ingress.sv" \
       "${ROOT_DIR}/rtl/src/frontend/sample_history.sv" \
       "${ROOT_DIR}/rtl/src/sync/null_detector.sv" \
       "${ROOT_DIR}/rtl/src/sync/prs_score_search.sv" \
       "${ROOT_DIR}/rtl/src/sync/cp_metric_engine.sv" \
       "${ROOT_DIR}/rtl/src/sync/prs_pilot_refiner.sv" \
       "${ROOT_DIR}/rtl/src/sync/prs_acquire_track.sv" \
       "${ROOT_DIR}/rtl/src/ofdm/symbol_buffer.sv" \
       "${ROOT_DIR}/rtl/src/ofdm/cfo_corrector.sv" \
       "${ROOT_DIR}/rtl/src/ofdm/fft_wrapper.sv" \
       "${ROOT_DIR}/rtl/src/ofdm/carrier_mapper.sv" \
       "${ROOT_DIR}/rtl/src/ofdm/prs_channel_estimator.sv" \
       "${ROOT_DIR}/rtl/src/ofdm/equalizer.sv" \
       "${ROOT_DIR}/rtl/src/ofdm/diff_demod.sv" \
       "${ROOT_DIR}/rtl/src/ofdm/frequency_deinterleaver.sv" \
       "${ROOT_DIR}/rtl/src/ofdm/qpsk_softbits.sv" \
       "${ROOT_DIR}/rtl/src/ofdm/tframe_assembler.sv" \
       "${ROOT_DIR}/rtl/src/top/dab_pipeline_top.sv" \
  --exe "${ROOT_DIR}/rtl/sim/tb/tb_dab_pipeline_top.cpp" \
  --top-module dab_pipeline_top \
  -Mdir "${BUILD_DIR}" \
  --build

"${BUILD_DIR}/Vdab_pipeline_top"
