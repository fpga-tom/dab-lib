#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD_DIR="${ROOT_DIR}/build/rtl/symbol_buffer"

mkdir -p "${BUILD_DIR}"

verilator \
  -Wall \
  -GFFT_SAMPLES=16 \
  --cc "${ROOT_DIR}/rtl/src/ofdm/symbol_buffer.sv" \
  --exe "${ROOT_DIR}/rtl/sim/tb/tb_symbol_buffer.cpp" \
  --top-module symbol_buffer \
  -Mdir "${BUILD_DIR}" \
  --build

"${BUILD_DIR}/Vsymbol_buffer"
