#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD_DIR="${ROOT_DIR}/build/rtl/prs_score_search"

mkdir -p "${BUILD_DIR}"

verilator \
  -Wall \
  --cc "${ROOT_DIR}/rtl/src/sync/prs_score_search.sv" \
  --exe "${ROOT_DIR}/rtl/sim/tb/tb_prs_score_search.cpp" \
  --top-module prs_score_search \
  -Mdir "${BUILD_DIR}" \
  --build

"${BUILD_DIR}/Vprs_score_search"
