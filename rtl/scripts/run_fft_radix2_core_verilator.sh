#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD_DIR="${ROOT_DIR}/build/rtl/fft_radix2_core"

mkdir -p "${BUILD_DIR}"

verilator \
  -Wall \
  --cc "${ROOT_DIR}/rtl/src/common/dab_ofdm_pkg.sv" \
       "${ROOT_DIR}/rtl/src/ofdm/fft_radix2_core.sv" \
  --exe "${ROOT_DIR}/rtl/sim/tb/tb_fft_radix2_core.cpp" \
  --top-module fft_radix2_core \
  -Mdir "${BUILD_DIR}" \
  --build

"${BUILD_DIR}/Vfft_radix2_core"
