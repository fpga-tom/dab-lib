#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD_DIR="${ROOT_DIR}/build/rtl/cp_metric_engine"

mkdir -p "${BUILD_DIR}"

verilator \
  -Wall \
  -GGUARD_SAMPLES=8 \
  -GFFT_SAMPLES=32 \
  --cc "${ROOT_DIR}/rtl/src/sync/cp_metric_engine.sv" \
  --exe "${ROOT_DIR}/rtl/sim/tb/tb_cp_metric_engine.cpp" \
  --top-module cp_metric_engine \
  -Mdir "${BUILD_DIR}" \
  --build

"${BUILD_DIR}/Vcp_metric_engine"
