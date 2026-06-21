#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD_DIR="${ROOT_DIR}/build/rtl/equalizer"

mkdir -p "${BUILD_DIR}"

verilator \
  -Wall \
  --cc "${ROOT_DIR}/rtl/src/common/dab_ofdm_pkg.sv" \
       "${ROOT_DIR}/rtl/src/ofdm/equalizer.sv" \
  --exe "${ROOT_DIR}/rtl/sim/tb/tb_equalizer.cpp" \
  --top-module equalizer \
  -Mdir "${BUILD_DIR}" \
  --build

"${BUILD_DIR}/Vequalizer"
