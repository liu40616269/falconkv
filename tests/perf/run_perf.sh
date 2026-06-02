#!/usr/bin/env bash
#
# run_perf.sh — One-click launcher for FalconKV end-to-end performance tests.
#
# Usage:
#   ./run_perf.sh [config_file]
#
# Default config: tests/perf/perf_config.json (relative to script location)
#
# Steps:
#   1. Parse config
#   2. Create directories
#   3. Start Meta → wait for port
#   4. Start Scheduler → wait for UDS
#   5. Launch all perf_client.py in parallel
#   6. Wait for all clients to finish
#   7. Stop Meta / Scheduler
#   8. Run perf_aggregate.py
#

set -euo pipefail

# ---------------------------------------------------------------------------
# Paths
# ---------------------------------------------------------------------------
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
BUILD_DIR="${PROJECT_ROOT}/build"

FALCONKV_MASTER="${BUILD_DIR}/src/meta/falconkv_master"
FALCONKV_SCHED="${BUILD_DIR}/src/scheduler/falconkv_sched"
PERF_DIR="${SCRIPT_DIR}"
PERF_CLIENT="${PERF_DIR}/perf_client.py"
PERF_AGGREGATE="${PERF_DIR}/perf_aggregate.py"

CONFIG_FILE="${1:-${PERF_DIR}/perf_config.json}"

# ---------------------------------------------------------------------------
# Parse config with python (jq may not be available)
# ---------------------------------------------------------------------------
parse_config() {
    python3 -c "
import json, sys
with open(sys.argv[1]) as f:
    c = json.load(f)
# Print key=value pairs, one per line (quote all values for bash eval safety)
t = c['test']
print(f'meta_listen_port={t.get(\"meta_listen_port\", 18900)}')
print(f'scheduler_uds_path=\"{t.get(\"scheduler_uds_path\", \"/tmp/falconkv_perf_sched.sock\")}\"')
print(f'result_dir=\"{t.get(\"result_dir\", \"/tmp/falconkv_perf_result\")}\"')
print(f'warmup_sec={t.get(\"warmup_sec\", 3)}')
print(f'duration_sec={t.get(\"duration_sec\", 30)}')

# Print client ids
ids = ' '.join(cl['client_id'] for cl in c['clients'])
print(f'client_ids=\"{ids}\"')

# Print ssd paths
for cl in c['clients']:
    print(f'ssd_path_{cl[\"client_id\"]}=\"{cl[\"ssd_path\"]}\"')

# Shard count
print(f'meta_shard_count={c.get(\"meta\", {}).get(\"shard_count\", 16)}')
# Prefer scheduler.enabled from test config; fall back to common.scheduler_enabled
_se = c.get('scheduler', {}).get('enabled', None)
if _se is None:
    _se = c.get('common', {}).get('scheduler_enabled', False)
sched_enabled = str(_se).lower()
print(f'scheduler_enabled=\"{sched_enabled}\"')
print(f'schedule_policy=\"{c.get(\"scheduler\", {}).get(\"schedule_policy\", \"passthrough\")}\"')
" "$CONFIG_FILE"
}

if [ ! -f "$CONFIG_FILE" ]; then
    echo "ERROR: Config file not found: ${CONFIG_FILE}"
    exit 1
fi

echo "=== FalconKV Performance Test ==="
echo "Config: ${CONFIG_FILE}"

# Parse into shell variables
eval "$(parse_config)"

# ---------------------------------------------------------------------------
# Helper: wait for TCP port
# ---------------------------------------------------------------------------
wait_for_port() {
    local host="$1" port="$2" timeout="${3:-15}"
    echo -n "  Waiting for port ${port} ..."
    local deadline=$(($(date +%s) + timeout))
    while [ "$(date +%s)" -lt "$deadline" ]; do
        if python3 -c "
import socket, sys
s = socket.create_connection(('${host}', ${port}), timeout=1)
s.close()
" 2>/dev/null; then
            echo " OK"
            return 0
        fi
        sleep 0.3
        echo -n "."
    done
    echo " TIMEOUT"
    return 1
}

# ---------------------------------------------------------------------------
# Helper: wait for file to exist
# ---------------------------------------------------------------------------
wait_for_file() {
    local path="$1" timeout="${2:-15}"
    echo -n "  Waiting for ${path} ..."
    local deadline=$(($(date +%s) + timeout))
    while [ "$(date +%s)" -lt "$deadline" ]; do
        if [ -e "$path" ]; then
            echo " OK"
            return 0
        fi
        sleep 0.3
        echo -n "."
    done
    echo " TIMEOUT"
    return 1
}

# ---------------------------------------------------------------------------
# Helper: stop a process with SIGTERM, then SIGKILL on timeout
# ---------------------------------------------------------------------------
stop_process() {
    local pid="$1"
    local name="${2:-process}"
    if [ -z "$pid" ] || ! kill -0 "$pid" 2>/dev/null; then
        return 0
    fi
    # Send SIGTERM
    kill "$pid" 2>/dev/null || true
    # Wait up to 5 seconds for graceful exit
    local deadline=$(($(date +%s) + 5))
    while [ "$(date +%s)" -lt "$deadline" ]; do
        if ! kill -0 "$pid" 2>/dev/null; then
            return 0
        fi
        sleep 0.2
    done
    # Force kill
    echo "  ${name} (PID ${pid}) did not exit, sending SIGKILL ..."
    kill -9 "$pid" 2>/dev/null || true
    # Brief wait for SIGKILL to take effect
    sleep 0.5
}

# ---------------------------------------------------------------------------
# Cleanup
# ---------------------------------------------------------------------------
META_PID=""
SCHED_PID=""
CLIENT_PIDS=""

cleanup() {
    echo ""
    echo "Cleaning up ..."
    # Kill client processes
    for pid in $CLIENT_PIDS; do
        kill "$pid" 2>/dev/null || true
    done
    # Stop scheduler
    if [ -n "$SCHED_PID" ]; then
        stop_process "$SCHED_PID" "Scheduler"
    fi
    # Stop meta
    if [ -n "$META_PID" ]; then
        stop_process "$META_PID" "Meta"
    fi
    # Clean up UDS file
    if [ -n "${scheduler_uds_path:-}" ] && [ -e "${scheduler_uds_path}" ]; then
        rm -f "${scheduler_uds_path}"
    fi
    echo "Cleanup done."
}

trap cleanup EXIT

# ---------------------------------------------------------------------------
# Create directories
# ---------------------------------------------------------------------------
echo ""
echo "Creating directories ..."

for cid in ${client_ids}; do
    ssd_var="ssd_path_${cid}"
    ssd="${!ssd_var}"
    mkdir -p "$ssd"
    echo "  SSD: ${ssd}"
done
mkdir -p "${result_dir}"
echo "  Results: ${result_dir}"

# ---------------------------------------------------------------------------
# Start Meta
# ---------------------------------------------------------------------------
echo ""
echo "Starting Meta server ..."

META_PORT="${meta_listen_port}"
META_CONFIG_DIR=$(mktemp -d /tmp/falconkv_perf_meta.XXXXXX)
META_CONFIG="${META_CONFIG_DIR}/meta_config.json"

cat > "${META_CONFIG}" <<EOF
{
  "common": {
    "log_dir": "${result_dir}/logs"
  },
  "meta": {
    "listen_addr": "0.0.0.0:${META_PORT}",
    "shard_count": ${meta_shard_count}
  }
}
EOF

mkdir -p "${result_dir}/logs"

if [ ! -x "${FALCONKV_MASTER}" ]; then
    echo "ERROR: falconkv_master not found at ${FALCONKV_MASTER}"
    echo "Please build first: ./build.sh build --with-python"
    exit 1
fi

"${FALCONKV_MASTER}" "${META_CONFIG}" > "${result_dir}/logs/meta_stdout.log" 2>&1 &
META_PID=$!

wait_for_port "127.0.0.1" "${META_PORT}" 15

# ---------------------------------------------------------------------------
# Start Scheduler (optional)
# ---------------------------------------------------------------------------
if [ "${scheduler_enabled}" = "True" ] || [ "${scheduler_enabled}" = "true" ]; then
    echo ""
    echo "Starting Scheduler server ..."

    SCHED_CONFIG_DIR=$(mktemp -d /tmp/falconkv_perf_sched.XXXXXX)
    SCHED_CONFIG="${SCHED_CONFIG_DIR}/sched_config.json"

    cat > "${SCHED_CONFIG}" <<EOF
{
  "common": {
    "log_dir": "${result_dir}/logs",
    "scheduler_uds_path": "${scheduler_uds_path}"
  },
  "scheduler": {
    "schedule_policy": "${schedule_policy}",
    "stats_report_interval_sec": 2
  }
}
EOF

    if [ ! -x "${FALCONKV_SCHED}" ]; then
        echo "ERROR: falconkv_sched not found at ${FALCONKV_SCHED}"
        exit 1
    fi

    "${FALCONKV_SCHED}" "${SCHED_CONFIG}" > "${result_dir}/logs/sched_stdout.log" 2>&1 &
    SCHED_PID=$!

    wait_for_file "${scheduler_uds_path}" 15
else
    echo ""
    echo "Scheduler disabled, skipping."
fi

# ---------------------------------------------------------------------------
# Launch clients in parallel
# ---------------------------------------------------------------------------
echo ""
echo "Launching ${client_ids} client(s) ..."

for cid in ${client_ids}; do
    echo "  Starting client ${cid} ..."
    python3 "${PERF_CLIENT}" --config "${CONFIG_FILE}" --client-id "${cid}" \
        > "${result_dir}/logs/client_${cid}.log" 2>&1 &
    CLIENT_PIDS="${CLIENT_PIDS} $!"
done

# ---------------------------------------------------------------------------
# Wait for all clients
# ---------------------------------------------------------------------------
echo ""
echo "Waiting for clients to finish (${duration_sec}s benchmark) ..."
FAIL=0
for pid in ${CLIENT_PIDS}; do
    if ! wait "$pid"; then
        echo "  WARNING: Client PID ${pid} exited with error"
        FAIL=1
    fi
done

if [ "$FAIL" -ne 0 ]; then
    echo ""
    echo "WARNING: Some clients failed. Check logs in ${result_dir}/logs/"
    echo ""
fi

# ---------------------------------------------------------------------------
# Stop services
# ---------------------------------------------------------------------------
echo "Stopping services ..."

if [ -n "$SCHED_PID" ]; then
    stop_process "$SCHED_PID" "Scheduler"
    SCHED_PID=""
fi

stop_process "$META_PID" "Meta"
META_PID=""

# ---------------------------------------------------------------------------
# Aggregate results
# ---------------------------------------------------------------------------
echo ""
echo "Aggregating results ..."
python3 "${PERF_AGGREGATE}" --result-dir "${result_dir}" --client-ids "${client_ids}"

echo ""
echo "Done. Results in ${result_dir}/"
