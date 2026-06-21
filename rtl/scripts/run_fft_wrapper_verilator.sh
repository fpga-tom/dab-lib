#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD_DIR="${ROOT_DIR}/build/rtl/fft_wrapper"

mkdir -p "${BUILD_DIR}"

verilator \
  -Wall \
  --cc "${ROOT_DIR}/rtl/src/common/dab_ofdm_pkg.sv" \
       "${ROOT_DIR}/rtl/src/ofdm/fft_wrapper.sv" \
  --exe "${ROOT_DIR}/rtl/sim/tb/tb_fft_wrapper.cpp" \
  --top-module fft_wrapper \
  -Mdir "${BUILD_DIR}" \
  --build

"${BUILD_DIR}/Vfft_wrapper"
