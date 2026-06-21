#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD_DIR="${ROOT_DIR}/build/rtl/prs_channel_estimator"

mkdir -p "${BUILD_DIR}"

verilator \
  -Wall \
  --cc "${ROOT_DIR}/rtl/src/common/dab_ofdm_pkg.sv" \
       "${ROOT_DIR}/rtl/src/ofdm/prs_channel_estimator.sv" \
  --exe "${ROOT_DIR}/rtl/sim/tb/tb_prs_channel_estimator.cpp" \
  --top-module prs_channel_estimator \
  -Mdir "${BUILD_DIR}" \
  --build

"${BUILD_DIR}/Vprs_channel_estimator"
