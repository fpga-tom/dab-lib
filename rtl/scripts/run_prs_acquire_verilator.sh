#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD_DIR="${ROOT_DIR}/build/rtl/prs_acquire"

mkdir -p "${BUILD_DIR}"

verilator \
  -Wall \
  -GACQUIRE_WINDOW_SAMPLES=1201 \
  --cc "${ROOT_DIR}/rtl/src/sync/prs_acquire.sv" \
  --exe "${ROOT_DIR}/rtl/sim/tb/tb_prs_acquire.cpp" \
  --top-module prs_acquire \
  -Mdir "${BUILD_DIR}" \
  --build

"${BUILD_DIR}/Vprs_acquire"
