"""
End-to-end integration tests for Store eviction and Scheduler statistics.

TestStoreEviction:
  - Writes enough keys to fill a store beyond the high watermark.
  - Waits for EvictManager to reclaim cold entries.
  - Verifies new keys can be written and old evicted keys are gone.

TestSchedulerStats:
  - Starts a Scheduler process with short stats_report_interval_sec.
  - Uses pyfalconkv.Client (with scheduler_enabled=true) to write and read data.
  - Client's internal SchedulerProxy sends IO reports to the Scheduler.
  - Verifies the Scheduler log contains throughput statistics for each channel.
"""

import ctypes
import json
import os
import re
import subprocess
import time
import tempfile
import shutil

import pytest

pytestmark = pytest.mark.integration


# ---------------------------------------------------------------------------
# Build paths
# ---------------------------------------------------------------------------
BUILD_DIR = os.environ.get(
    "FALCONKV_BUILD_DIR",
    os.path.join(os.path.dirname(__file__), "..", "..", "build"),
)
BUILD_DIR = os.path.abspath(BUILD_DIR)


# ---------------------------------------------------------------------------
# Helpers — buffer management (same as test_functional_e2e.py)
# ---------------------------------------------------------------------------

class BufferGuard:
    """Holds a ctypes buffer alive and exposes its address/size."""

    def __init__(self, data: bytes):
        self._buf = (ctypes.c_ubyte * len(data))(*data)
        self.ptr = ctypes.addressof(self._buf)
        self.size = len(data)

    def read_bytes(self):
        return bytes((ctypes.c_ubyte * self.size).from_address(self.ptr))


class ZeroBuffer:
    """Allocated zeroed buffer of given size."""

    def __init__(self, size: int):
        self._buf = (ctypes.c_ubyte * size)()
        self.ptr = ctypes.addressof(self._buf)
        self.size = size

    def read_bytes(self, nbytes=None):
        n = nbytes if nbytes is not None else self.size
        return bytes((ctypes.c_ubyte * n).from_address(self.ptr))


def _can_import_client():
    try:
        from pyfalconkv.client import Client  # noqa: F401
        return True
    except Exception:
        return False


# ---------------------------------------------------------------------------
# Helper — find scheduler log in test_log_dir
# ---------------------------------------------------------------------------

def _find_scheduler_log(test_log_dir):
    """Find the falconkv_sched log file in test_log_dir.

    The scheduler writes to <log_dir>/falconkv_sched_<pid>_<timestamp>.log
    via InitSharedLogging -> freopen(stderr).

    Only matches "falconkv_sched_" followed by a PID (digits), excluding
    falconkv_sched_reporter logs.

    Returns the most recently modified matching file.
    """
    candidates = []
    for f in os.listdir(test_log_dir):
        if re.match(r"falconkv_sched_\d+_", f) and f.endswith(".log"):
            candidates.append(f)
    if not candidates:
        return None
    # Return the most recently modified file.
    candidates.sort(key=lambda f: os.path.getmtime(os.path.join(test_log_dir, f)))
    return os.path.join(test_log_dir, candidates[-1])


def _grep_log(log_path, pattern):
    """Search for pattern in a log file, return matching lines."""
    with open(log_path) as f:
        return [line for line in f if re.search(pattern, line)]


# ---------------------------------------------------------------------------
# Helper — create evict client
# ---------------------------------------------------------------------------

def _make_evict_client(ssd_dir, log_dir="/tmp/falconkv_log", meta_addr="localhost:19999"):
    """Create a pyfalconkv.Client with 1 GB capacity and fast eviction params."""
    from pyfalconkv.client import Client

    config = {
        "common": {
            "meta_addr": meta_addr,
            "log_dir": log_dir,
            "scheduler_enabled": False,
            "scheduler_uds_path": "/tmp/nonexistent.sock",
        },
        "store": {
            "ssd_path": ssd_dir,
            "store_id": 1,
            "node_id": 1,
            "capacity_gb": 1,                # 1 GB
            "page_size": 4096,
            "io_threads": 2,
            "store_rpc_host": "127.0.0.1",
            "listen_port": 18901,
            "io_uring_enabled": False,
            "direct_io_enabled": False,
            "evict_grace_period_ms": 1000,
            "evict_check_interval_sec": 1,
            "evict_high_watermark": 0.80,
            "evict_low_watermark": 0.50,
            "evict_cold_threshold_ms": 2000,
        },
        "client": {
            "cache_capacity": 100000,
            "scheduler_enabled": False,
        },
        "scheduler": {"uds_path": "/tmp/nonexistent.sock"},
        "transfer": {"meta_addr": meta_addr},
    }
    config_path = os.path.join(ssd_dir, "evict_config.json")
    os.makedirs(ssd_dir, exist_ok=True)
    with open(config_path, "w") as f:
        json.dump(config, f)
    return Client(config_path, cache_capacity=100000)


# ---------------------------------------------------------------------------
# Helper — create scheduler-enabled client for stats tests
# ---------------------------------------------------------------------------

def _make_scheduler_client(ssd_dir, meta_addr, scheduler_uds_path,
                           store_id=1, node_id=1, listen_port=18901,
                           log_dir="/tmp/falconkv_log"):
    """Create a pyfalconkv.Client with scheduler enabled."""
    from pyfalconkv.client import Client

    config = {
        "common": {
            "meta_addr": meta_addr,
            "node_id": node_id,
            "scheduler_enabled": True,
            "scheduler_uds_path": scheduler_uds_path,
            "log_dir": log_dir,
        },
        "store": {
            "ssd_path": ssd_dir,
            "store_id": store_id,
            "node_id": node_id,
            "capacity_gb": 1,
            "page_size": 4096,
            "io_threads": 2,
            "store_rpc_host": "127.0.0.1",
            "listen_port": listen_port,
        },
        "client": {
            "cache_capacity": 100000,
            "scheduler_enabled": True,
            "scheduler_uds_path": scheduler_uds_path,
        },
        "scheduler": {
            "uds_path": scheduler_uds_path,
        },
        "transfer": {"meta_addr": meta_addr},
    }
    config_path = os.path.join(ssd_dir, f"sched_client_{store_id}.json")
    os.makedirs(ssd_dir, exist_ok=True)
    with open(config_path, "w") as f:
        json.dump(config, f)
    return Client(config_path, cache_capacity=100000)


# ===========================================================================
# Test 1: Store eviction
# ===========================================================================

@pytest.mark.skipif(not _can_import_client(), reason="pyfalconkv not available")
class TestStoreEviction:
    """Store capacity exhausted -> EvictManager reclaims cold entries -> new writes succeed."""

    def test_evict_frees_space_for_new_writes(self, temp_ssd_dir, test_log_dir, meta_server):
        """Write keys until the store is full, wait for eviction, then verify new keys can be written.

        Steps:
        1. Create a 1 GB client with fast eviction params.
        2. Phase 1: Write ~830 unique keys (1 MB each) to exceed the 80% high watermark.
        3. Wait for cold_threshold (2 s) + check_interval (1 s) + grace_period (1 s) = 4 s.
        4. Phase 2: Write new keys and verify they succeed (space was reclaimed).
        5. Verify some old keys are no longer readable (evicted).
        """
        client = _make_evict_client(temp_ssd_dir, log_dir=test_log_dir,
                                    meta_addr=meta_server["addr"])
        try:
            value_size = 1024 * 1024  # 1 MB per key
            # 1 GB / 1 MB = 1024 pages (slot allocator units).
            # High watermark 80% = ~819 pages.  Write ~830 keys to exceed it.
            phase1_count = 830
            phase1_keys = []

            for i in range(phase1_count):
                key = f"evict_phase1_{i:05d}"
                data = bytes([i % 256]) * value_size
                w = BufferGuard(data)
                try:
                    client.batch_put_sync([key], [w.ptr], [w.size])
                except RuntimeError:
                    # Store is full — stop writing.
                    break
                phase1_keys.append((key, w))

            # Must have written enough to exceed high watermark.
            assert len(phase1_keys) >= 600, (
                f"Only wrote {len(phase1_keys)} keys before full, "
                f"expected >= 600 to trigger eviction"
            )

            # Wait for eviction to kick in:
            #   cold_threshold (2s) + check_interval (1s) + grace_period (1s) + margin (2s)
            time.sleep(6)

            # Phase 2: Write new keys — should succeed after eviction frees space.
            phase2_data = os.urandom(value_size)
            w2 = BufferGuard(phase2_data)
            phase2_key = "evict_phase2_new_00000"
            client.batch_put_sync([phase2_key], [w2.ptr], [w2.size])

            # Read back the new key.
            r = ZeroBuffer(value_size)
            results = client.batch_get_sync([phase2_key], [r.ptr], [r.size])
            assert results[0] == value_size, "Phase-2 key should be readable"
            assert r.read_bytes() == phase2_data, "Phase-2 data mismatch"

            # Verify at least some old keys are gone (evicted).
            # Use batch_exist_sync which queries the store's meta_index directly
            # (not the client's KeyDescCache), so evicted keys are correctly
            # reported as missing.
            missing = 0
            check_count = min(100, len(phase1_keys))
            check_keys = [key for key, _ in phase1_keys[:check_count]]
            exist_count = client.batch_exist_sync(check_keys)
            missing = check_count - exist_count
            assert missing > 0, (
                f"Expected some phase-1 keys to be evicted, but all {check_count} are still present"
            )
        finally:
            client.close()


# ===========================================================================
# Test 2: Scheduler statistics — local SSD read/write + net_tx_read
# ===========================================================================

@pytest.mark.skipif(not _can_import_client(), reason="pyfalconkv not available")
class TestSchedulerStats:
    """Scheduler tracks IO throughput; stats appear in the scheduler log."""

    def test_local_rw_stats_in_logs(self, temp_ssd_dir, test_log_dir,
                                     meta_server, scheduler_server):
        """Write and read data via Client with scheduler_enabled=true.

        Client's internal SchedulerProxy sends RequestIO + ReportIOCompletion
        RPCs to the Scheduler for each put (LOCAL_SSD_WRITE) and get
        (LOCAL_SSD_READ).  Verify scheduler log contains throughput stats.
        """
        uds_path = scheduler_server["uds_path"]
        meta_addr = meta_server["addr"]

        client = _make_scheduler_client(
            temp_ssd_dir, meta_addr, uds_path,
            store_id=1, node_id=1, listen_port=18901,
            log_dir=test_log_dir,
        )

        try:
            # Write 20 keys (1 KB each) to trigger LOCAL_SSD_WRITE reports.
            value_size = 1024
            for i in range(20):
                key = f"sched_write_{i:04d}"
                data = bytes([i % 256]) * value_size
                w = BufferGuard(data)
                client.batch_put_sync([key], [w.ptr], [w.size])

            # Read them back to trigger LOCAL_SSD_READ reports.
            for i in range(20):
                key = f"sched_write_{i:04d}"
                r = ZeroBuffer(value_size)
                client.batch_get_sync([key], [r.ptr], [r.size])

            # Wait for the scheduler to print a stats report.
            # stats_report_interval_sec=2, wait a bit longer for safety.
            time.sleep(4)

            # Find and check the scheduler log.
            log_path = _find_scheduler_log(test_log_dir)
            assert log_path is not None, (
                f"No falconkv_sched log found in {test_log_dir}. "
                f"Contents: {os.listdir(test_log_dir)}"
            )

            # Verify local_ssd_write stats (ios > 0).
            write_lines = _grep_log(log_path, r"local_ssd_write.*ios=(\d+)")
            assert len(write_lines) > 0, (
                "No local_ssd_write stats found in scheduler log"
            )
            m = re.search(r"ios=(\d+)", write_lines[0])
            assert m is not None
            write_ios = int(m.group(1))
            assert write_ios > 0, f"Expected ios > 0 for local_ssd_write, got {write_ios}"

            # Verify local_ssd_read stats (ios > 0).
            read_lines = _grep_log(log_path, r"local_ssd_read.*ios=(\d+)")
            assert len(read_lines) > 0, (
                "No local_ssd_read stats found in scheduler log"
            )
            m = re.search(r"ios=(\d+)", read_lines[0])
            assert m is not None
            read_ios = int(m.group(1))
            assert read_ios > 0, f"Expected ios > 0 for local_ssd_read, got {read_ios}"

        finally:
            client.close()

    def test_net_tx_read_stats_in_logs(self, temp_ssd_dir, test_log_dir,
                                        meta_server, scheduler_server):
        """Cross-store read triggers NET_TX_READ IO report.

        Sets up two stores (store_id=1 on node_id=1, store_id=2 on node_id=2)
        on the same machine.  Client A writes to Store A; Client B reads from
        Store B, which triggers a remote read from Store A (ACCESS_REMOTE_RPC)
        and the Client B SchedulerProxy sends a NET_TX_READ IO report.
        """
        uds_path = scheduler_server["uds_path"]
        meta_addr = meta_server["addr"]

        # Two separate SSD directories.
        ssd_dir_a = os.path.join(temp_ssd_dir, "store_a")
        ssd_dir_b = os.path.join(temp_ssd_dir, "store_b")
        os.makedirs(ssd_dir_a, exist_ok=True)
        os.makedirs(ssd_dir_b, exist_ok=True)

        # Client A: Store 1, Node 1, Port 18901.
        client_a = _make_scheduler_client(
            ssd_dir_a, meta_addr, uds_path,
            store_id=1, node_id=1, listen_port=18901,
            log_dir=test_log_dir,
        )

        # Client B: Store 2, Node 2, Port 18912.
        client_b = _make_scheduler_client(
            ssd_dir_b, meta_addr, uds_path,
            store_id=2, node_id=2, listen_port=18912,
            log_dir=test_log_dir,
        )

        try:
            # Write data via Client A → Store 1 (node 1).
            value_size = 4096
            key = "sched_net_tx_test"
            data = bytes(range(256)) * 16  # 4096 bytes
            w = BufferGuard(data)
            client_a.batch_put_sync([key], [w.ptr], [w.size])

            # Wait for Store 1 to sync metadata to Meta.
            time.sleep(1)

            # Read the same key via Client B → triggers remote read from Store 1.
            r = ZeroBuffer(value_size)
            results = client_b.batch_get_sync([key], [r.ptr], [r.size])

            # The read may succeed or fail depending on network setup.
            # Even if the data doesn't match, the SchedulerProxy should have
            # sent at least one NET_TX_READ RequestIO + ReportIOCompletion.

            # Wait for the scheduler to print a stats report.
            time.sleep(4)

            # Find and check the scheduler log.
            log_path = _find_scheduler_log(test_log_dir)
            assert log_path is not None, (
                f"No falconkv_sched log found in {test_log_dir}. "
                f"Contents: {os.listdir(test_log_dir)}"
            )

            # Verify net_tx_read stats appear in the log.
            tx_lines = _grep_log(log_path, r"net_tx_read.*ios=(\d+)")
            assert len(tx_lines) > 0, (
                "No net_tx_read stats found in scheduler log"
            )
            m = re.search(r"ios=(\d+)", tx_lines[0])
            assert m is not None
            tx_ios = int(m.group(1))
            assert tx_ios > 0, f"Expected ios > 0 for net_tx_read, got {tx_ios}"

        finally:
            client_b.close()
            client_a.close()
