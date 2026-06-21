#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD_DIR="${ROOT_DIR}/build/rtl/tframe_assembler"

mkdir -p "${BUILD_DIR}"

verilator \
  -Wall \
  --cc "${ROOT_DIR}/rtl/src/common/dab_tframe_pkg.sv" \
       "${ROOT_DIR}/rtl/src/ofdm/tframe_assembler.sv" \
  --exe "${ROOT_DIR}/rtl/sim/tb/tb_tframe_assembler.cpp" \
  --top-module tframe_assembler \
  -Mdir "${BUILD_DIR}" \
  --build

"${BUILD_DIR}/Vtframe_assembler"
