#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD_DIR="${ROOT_DIR}/build/rtl/carrier_mapper"

mkdir -p "${BUILD_DIR}"

verilator \
  -Wall \
  --cc "${ROOT_DIR}/rtl/src/common/dab_ofdm_pkg.sv" \
       "${ROOT_DIR}/rtl/src/ofdm/carrier_mapper.sv" \
  --exe "${ROOT_DIR}/rtl/sim/tb/tb_carrier_mapper.cpp" \
  --top-module carrier_mapper \
  -Mdir "${BUILD_DIR}" \
  --build

"${BUILD_DIR}/Vcarrier_mapper"
