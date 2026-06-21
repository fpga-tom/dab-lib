#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD_DIR="${ROOT_DIR}/build/rtl/prs_acquire_track"

mkdir -p "${BUILD_DIR}"

verilator \
  -Wall \
  --cc "${ROOT_DIR}/rtl/src/sync/prs_acquire_track.sv" \
  --exe "${ROOT_DIR}/rtl/sim/tb/tb_prs_acquire_track.cpp" \
  --top-module prs_acquire_track \
  -Mdir "${BUILD_DIR}" \
  --build

"${BUILD_DIR}/Vprs_acquire_track"
