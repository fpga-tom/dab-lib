#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD_DIR="${ROOT_DIR}/build/rtl/prs_pilot_refiner"

mkdir -p "${BUILD_DIR}"

verilator \
  -Wall \
  --cc "${ROOT_DIR}/rtl/src/common/dab_ofdm_pkg.sv" \
       "${ROOT_DIR}/rtl/src/sync/prs_pilot_refiner.sv" \
  --exe "${ROOT_DIR}/rtl/sim/tb/tb_prs_pilot_refiner.cpp" \
  --top-module prs_pilot_refiner \
  -Mdir "${BUILD_DIR}" \
  --build

"${BUILD_DIR}/Vprs_pilot_refiner"
