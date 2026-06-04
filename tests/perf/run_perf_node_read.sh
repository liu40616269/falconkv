#!/usr/bin/env bash
#
# run_perf_node_read.sh — Same-node read performance test.
#
# Tests ACCESS_NODE_DIRECT:
#   Client A writes data (node 1, store 1).
#   Client B reads data from the same node (node 1, store 2) via DirectIO.
#
# Usage:
#   ./run_perf_node_read.sh [config_file]
#

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "${SCRIPT_DIR}/perf_common.sh"

CONFIG_FILE="${1:-${SCRIPT_DIR}/perf_config_node_read.json}"
run_perf "$CONFIG_FILE" "FalconKV Same-Node Read Test"
