#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD_DIR="${ROOT_DIR}/build/rtl/diff_demod"

mkdir -p "${BUILD_DIR}"

verilator \
  -Wall \
  --cc "${ROOT_DIR}/rtl/src/common/dab_ofdm_pkg.sv" \
       "${ROOT_DIR}/rtl/src/ofdm/diff_demod.sv" \
  --exe "${ROOT_DIR}/rtl/sim/tb/tb_diff_demod.cpp" \
  --top-module diff_demod \
  -Mdir "${BUILD_DIR}" \
  --build

"${BUILD_DIR}/Vdiff_demod"
