#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD_DIR="${ROOT_DIR}/build/rtl/null_detector"

mkdir -p "${BUILD_DIR}"

verilator \
  -Wall \
  -GFAST_WINDOW_SAMPLES=8 \
  -GSLOW_WINDOW_SAMPLES=64 \
  -GENTER_THRESHOLD_NUM=2 \
  -GENTER_THRESHOLD_DEN=5 \
  -GEXIT_THRESHOLD_NUM=3 \
  -GEXIT_THRESHOLD_DEN=5 \
  -GMIN_NULL_SAMPLES=24 \
  -GMAX_NULL_SAMPLES=96 \
  -GMAX_MERGE_GAP_SAMPLES=4 \
  --cc "${ROOT_DIR}/rtl/src/sync/null_detector.sv" \
  --exe "${ROOT_DIR}/rtl/sim/tb/tb_null_detector.cpp" \
  --top-module null_detector \
  -Mdir "${BUILD_DIR}" \
  --build

"${BUILD_DIR}/Vnull_detector"
