#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD_DIR="${ROOT_DIR}/build/rtl/frequency_deinterleaver"

mkdir -p "${BUILD_DIR}"

verilator \
  -Wall \
  --cc "${ROOT_DIR}/rtl/src/common/dab_ofdm_pkg.sv" \
       "${ROOT_DIR}/rtl/src/ofdm/frequency_deinterleaver.sv" \
  --exe "${ROOT_DIR}/rtl/sim/tb/tb_frequency_deinterleaver.cpp" \
  --top-module frequency_deinterleaver \
  -Mdir "${BUILD_DIR}" \
  --build

"${BUILD_DIR}/Vfrequency_deinterleaver"
