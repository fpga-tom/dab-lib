#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD_DIR="${ROOT_DIR}/build/rtl/sample_history"

mkdir -p "${BUILD_DIR}"

verilator \
  -Wall \
  -GDEPTH_SAMPLES=16 \
  --cc "${ROOT_DIR}/rtl/src/frontend/sample_history.sv" \
  --exe "${ROOT_DIR}/rtl/sim/tb/tb_sample_history.cpp" \
  --top-module sample_history \
  -Mdir "${BUILD_DIR}" \
  --build

"${BUILD_DIR}/Vsample_history"
