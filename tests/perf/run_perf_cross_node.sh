#!/usr/bin/env bash
#
# run_perf_cross_node.sh — Cross-node read performance test.
#
# Tests ACCESS_REMOTE_RPC:
#   Client A writes data (node 1, store 1).
#   Client C reads data from a different node (node 2, store 3) via RPC.
#
# Usage:
#   ./run_perf_cross_node.sh [config_file]
#

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "${SCRIPT_DIR}/perf_common.sh"

CONFIG_FILE="${1:-${SCRIPT_DIR}/perf_config_cross_node.json}"
run_perf "$CONFIG_FILE" "FalconKV Cross-Node Read Test"
