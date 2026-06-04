#!/usr/bin/env bash
#
# run_perf_local.sh — Local read/write performance test.
#
# Tests ACCESS_LOCAL_DIRECT:
#   Client A writes data to its local store, then reads it back in-process.
#   Only Client A participates (single client, single store).
#
# Usage:
#   ./run_perf_local.sh [config_file]
#

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "${SCRIPT_DIR}/perf_common.sh"

CONFIG_FILE="${1:-${SCRIPT_DIR}/perf_config_local.json}"
run_perf "$CONFIG_FILE" "FalconKV Local Read/Write Test"
