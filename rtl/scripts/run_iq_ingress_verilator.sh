#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD_DIR="${ROOT_DIR}/build/rtl/iq_ingress"

mkdir -p "${BUILD_DIR}"

verilator \
  -Wall \
  --cc "${ROOT_DIR}/rtl/src/frontend/iq_ingress.sv" \
  --exe "${ROOT_DIR}/rtl/sim/tb/tb_iq_ingress.cpp" \
  --top-module iq_ingress \
  -Mdir "${BUILD_DIR}" \
  --build

"${BUILD_DIR}/Viq_ingress"
