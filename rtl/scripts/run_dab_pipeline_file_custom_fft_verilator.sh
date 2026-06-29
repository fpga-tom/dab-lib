#!/usr/bin/env bash
set -euo pipefail

usage() {
  printf 'Usage: %s [tframes]\n' "${0##*/}"
  printf '\n'
  printf 'Build and run the file-driven DAB pipeline Verilator testbench using the custom RTL FFT.\n'
  printf '  tframes  Optional positive integer number of transmission frames to decode.\n'
  printf '\n'
  printf 'Without an argument, the testbench uses DAB_TB_TFRAMES or defaults to 1.\n'
  printf 'Defaults specific to this path: DAB_TB_SAMPLE_PERIOD_CYCLES=16, '
  printf 'DAB_TB_SOFTBITS_PATH=/tmp/dab_softbits_custom_fft_i8.bin.\n'
}

if [[ "${1:-}" == "-h" || "${1:-}" == "--help" ]]; then
  usage
  exit 0
fi

if (( $# > 1 )); then
  usage >&2
  exit 2
fi

if (( $# == 1 )); then
  if [[ ! "$1" =~ ^[1-9][0-9]*$ ]]; then
    printf 'error: tframes must be a positive integer\n' >&2
    exit 2
  fi
  export DAB_TB_TFRAMES="$1"
fi

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD_DIR="${ROOT_DIR}/build/rtl/dab_pipeline_file_custom_fft"
HISTORY_DEPTH="${DAB_TB_HISTORY_DEPTH:-32768}"
INTERNAL_DATA_SCHEDULER="${DAB_TB_INTERNAL_DATA_SCHEDULER:-1}"
STREAMING_CP_SEARCH="${DAB_TB_ENABLE_STREAMING_CP_SEARCH:-1}"
NULL_FAST_WINDOW_SAMPLES="${DAB_TB_NULL_FAST_WINDOW_SAMPLES:-39}"
NULL_SLOW_WINDOW_SAMPLES="${DAB_TB_NULL_SLOW_WINDOW_SAMPLES:-1999}"
NULL_MIN_SAMPLES="${DAB_TB_NULL_MIN_SAMPLES:-1800}"

export DAB_TB_SAMPLE_PERIOD_CYCLES="${DAB_TB_SAMPLE_PERIOD_CYCLES:-16}"
export DAB_TB_SOFTBITS_PATH="${DAB_TB_SOFTBITS_PATH:-/tmp/dab_softbits_custom_fft_i8.bin}"
export DAB_TB_MAX_CYCLES="${DAB_TB_MAX_CYCLES:-400000000}"

mkdir -p "${BUILD_DIR}"

verilator \
  -Wall \
  -CFLAGS "-std=c++17 -DDAB_TB_CUSTOM_FFT" \
  -GSAMPLE_HISTORY_DEPTH="${HISTORY_DEPTH}" \
  -GENABLE_INTERNAL_DATA_SCHEDULER="${INTERNAL_DATA_SCHEDULER}" \
  -GENABLE_STREAMING_CP_SEARCH="${STREAMING_CP_SEARCH}" \
  -GNULL_FAST_WINDOW_SAMPLES="${NULL_FAST_WINDOW_SAMPLES}" \
  -GNULL_SLOW_WINDOW_SAMPLES="${NULL_SLOW_WINDOW_SAMPLES}" \
  -GNULL_MIN_SAMPLES="${NULL_MIN_SAMPLES}" \
  --cc "${ROOT_DIR}/rtl/src/common/dab_ofdm_pkg.sv" \
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
  --exe "${ROOT_DIR}/rtl/sim/tb/tb_dab_pipeline_file.cpp" \
  --top-module dab_pipeline_top_custom_fft \
  -Mdir "${BUILD_DIR}" \
  --build

"${BUILD_DIR}/Vdab_pipeline_top_custom_fft"
