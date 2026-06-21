#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD_DIR="${ROOT_DIR}/build/rtl/qpsk_softbits"

mkdir -p "${BUILD_DIR}"

verilator \
  -Wall \
  --cc "${ROOT_DIR}/rtl/src/common/dab_ofdm_pkg.sv" \
       "${ROOT_DIR}/rtl/src/ofdm/qpsk_softbits.sv" \
  --exe "${ROOT_DIR}/rtl/sim/tb/tb_qpsk_softbits.cpp" \
  --top-module qpsk_softbits \
  -Mdir "${BUILD_DIR}" \
  --build

"${BUILD_DIR}/Vqpsk_softbits"
