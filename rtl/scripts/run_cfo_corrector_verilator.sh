#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD_DIR="${ROOT_DIR}/build/rtl/cfo_corrector"

mkdir -p "${BUILD_DIR}"

verilator \
  -Wall \
  --cc "${ROOT_DIR}/rtl/src/common/dab_ofdm_pkg.sv" \
       "${ROOT_DIR}/rtl/src/ofdm/cfo_corrector.sv" \
  --exe "${ROOT_DIR}/rtl/sim/tb/tb_cfo_corrector.cpp" \
  --top-module cfo_corrector \
  -Mdir "${BUILD_DIR}" \
  --build

"${BUILD_DIR}/Vcfo_corrector"
