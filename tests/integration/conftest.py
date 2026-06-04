"""
Shared pytest fixtures for FalconKV integration tests.

Provides fixtures to start/stop FalconKV services (Meta, Store, Scheduler)
and create connected Python clients for testing.

Also provides LMCache-related fixtures for RemoteBackend E2E tests.
"""

import asyncio
import json
import os
import signal
import socket
import subprocess
import time
import tempfile
import shutil
import threading
import pytest


# ---------------------------------------------------------------------------
# Binary paths (built in build/ directory)
# ---------------------------------------------------------------------------
BUILD_DIR = os.environ.get(
    "FALCONKV_BUILD_DIR",
    os.path.join(os.path.dirname(__file__), "..", "..", "build"),
)
BUILD_DIR = os.path.abspath(BUILD_DIR)

FALCONKV_MASTER = os.path.join(BUILD_DIR, "src", "meta", "falconkv_master")
FALCONKV_SCHED = os.path.join(BUILD_DIR, "src", "scheduler", "falconkv_sched")

# Default log directory for all integration test services (meta/scheduler/client).
FALCONKV_TEST_LOG_DIR = "/tmp/falconkv_log"


def _wait_for_port(host, port, timeout=10):
    """Wait until a TCP port is accepting connections."""
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            s = socket.create_connection((host, port), timeout=1)
            s.close()
            return True
        except (ConnectionRefusedError, OSError):
            time.sleep(0.2)
    return False


def _wait_for_file(path, timeout=10):
    """Wait until a file exists."""
    deadline = time.time() + timeout
    while time.time() < deadline:
        if os.path.exists(path):
            return True
        time.sleep(0.2)
    return False


def _write_config(path, config_dict):
    """Write a JSON config file."""
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w") as f:
        json.dump(config_dict, f, indent=2)


# ---------------------------------------------------------------------------
# Fixture: temporary directory
# ---------------------------------------------------------------------------
@pytest.fixture
def temp_ssd_dir():
    """Create a temporary directory for SSD data, cleaned up after test."""
    d = tempfile.mkdtemp(prefix="falconkv_ssd_")
    yield d
    shutil.rmtree(d, ignore_errors=True)


# ---------------------------------------------------------------------------
# Fixture: per-test-case log directory
# ---------------------------------------------------------------------------
@pytest.fixture
def test_log_dir(request):
    """Create a per-test-case log directory under FALCONKV_TEST_LOG_DIR.

    Structure: /tmp/falconkv_log/{file_stem}/{test_function_name}/
    """
    file_stem = os.path.splitext(os.path.basename(request.node.path))[0]
    test_name = request.node.name
    log_dir = os.path.join(FALCONKV_TEST_LOG_DIR, file_stem, test_name)
    os.makedirs(log_dir, exist_ok=True)
    return log_dir


# ---------------------------------------------------------------------------
# Fixture: Meta server
# ---------------------------------------------------------------------------
@pytest.fixture
def meta_server(temp_ssd_dir, test_log_dir):
    """Start and stop the FalconKV Meta server (falconkv_master).

    Yields a dict with connection info.
    """
    if not os.path.isfile(FALCONKV_MASTER):
        pytest.skip(f"falconkv_master not found at {FALCONKV_MASTER}")

    listen_port = 18900
    config = {
        "common": {
            "meta_addr": f"0.0.0.0:{listen_port}",
            "log_dir": test_log_dir,
        },
        "meta": {
            "shard_count": 16,
        },
    }
    config_file = os.path.join(temp_ssd_dir, "meta_config.json")
    _write_config(config_file, config)

    proc = subprocess.Popen(
        [FALCONKV_MASTER, config_file],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )

    if not _wait_for_port("127.0.0.1", listen_port, timeout=10):
        proc.kill()
        out, err = proc.communicate(timeout=5)
        pytest.skip(
            f"Meta server failed to start.\nstdout: {out[:500]}\nstderr: {err[:500]}"
        )

    yield {
        "host": "127.0.0.1",
        "port": listen_port,
        "addr": f"127.0.0.1:{listen_port}",
        "process": proc,
    }

    proc.send_signal(signal.SIGTERM)
    try:
        proc.wait(timeout=5)
    except subprocess.TimeoutExpired:
        proc.kill()
        proc.wait()


# ---------------------------------------------------------------------------
# Fixture: Scheduler server
# ---------------------------------------------------------------------------
@pytest.fixture
def scheduler_server(temp_ssd_dir, test_log_dir):
    """Start and stop the FalconKV Scheduler server (falconkv_sched).

    Yields a dict with connection info.
    """
    if not os.path.isfile(FALCONKV_SCHED):
        pytest.skip(f"falconkv_sched not found at {FALCONKV_SCHED}")

    uds_path = os.path.join(temp_ssd_dir, "scheduler.sock")
    config = {
        "common": {
            "log_dir": test_log_dir,
            "scheduler_uds_path": uds_path,
        },
        "scheduler": {
            "schedule_policy": "passthrough",
            "stats_report_interval_sec": 2,
        },
    }
    config_file = os.path.join(temp_ssd_dir, "sched_config.json")
    _write_config(config_file, config)

    proc = subprocess.Popen(
        [FALCONKV_SCHED, config_file],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )

    if not _wait_for_file(uds_path, timeout=10):
        proc.kill()
        out, err = proc.communicate(timeout=5)
        pytest.skip(
            f"Scheduler server failed to start.\nstdout: {out[:500]}\nstderr: {err[:500]}"
        )

    yield {
        "uds_path": uds_path,
        "process": proc,
    }

    proc.send_signal(signal.SIGTERM)
    try:
        proc.wait(timeout=5)
    except subprocess.TimeoutExpired:
        proc.kill()
        proc.wait()
    # Clean up stale UDS file.
    if os.path.exists(uds_path):
        os.remove(uds_path)


# ---------------------------------------------------------------------------
# Fixture: FalconKV Python client
# ---------------------------------------------------------------------------
@pytest.fixture
def falconkv_client(meta_server, temp_ssd_dir, test_log_dir):
    """Create a FalconKV Python client connected to the running Meta.

    Requires pyfalconkv to be installed. Yields a Client instance.
    """
    try:
        from pyfalconkv.client import Client
    except ImportError:
        pytest.skip("pyfalconkv not installed (build with --with-python)")

    config = {
        "common": {
            "meta_addr": meta_server["addr"],
            "scheduler_enabled": False,
            "log_dir": test_log_dir,
        },
        "client": {
            "cache_capacity": 10000,
        },
    }
    config_file = os.path.join(temp_ssd_dir, "client_config.json")
    _write_config(config_file, config)

    client = Client(config_file, cache_capacity=10000)
    yield client
    client.close()


# ---------------------------------------------------------------------------
# LMCache-related fixtures for RemoteBackend E2E tests
# ---------------------------------------------------------------------------

def _make_falconkv_config(
    ssd_dir,
    log_dir=FALCONKV_TEST_LOG_DIR,
    store_id=1,
    node_id=1,
    meta_addr="127.0.0.1:18900",
    capacity_gb=1,
    evict_grace_period_ms=None,
    evict_check_interval_sec=None,
    evict_high_watermark=None,
    evict_low_watermark=None,
    evict_cold_threshold_ms=None,
):
    """Generate a FalconKV JSON config file and return its path.

    The config includes store, client, meta, scheduler, and transfer sections
    suitable for creating a pyfalconkv.Client or FalconKVConnector.
    """
    # Assign a unique listen_port per store_id to avoid collisions
    listen_port = 18901 + (store_id - 1) * 10
    store_cfg = {
        "ssd_path": ssd_dir,
        "store_id": store_id,
        "capacity_gb": capacity_gb,
        "page_size": 4096,
        "io_threads": 2,
        "io_uring_enabled": False,
        "direct_io_enabled": True,
        "store_rpc_host": "127.0.0.1",
        "listen_port": listen_port,
    }
    # Conditionally add evict parameters when provided.
    if evict_grace_period_ms is not None:
        store_cfg["evict_grace_period_ms"] = evict_grace_period_ms
    if evict_check_interval_sec is not None:
        store_cfg["evict_check_interval_sec"] = evict_check_interval_sec
    if evict_high_watermark is not None:
        store_cfg["evict_high_watermark"] = evict_high_watermark
    if evict_low_watermark is not None:
        store_cfg["evict_low_watermark"] = evict_low_watermark
    if evict_cold_threshold_ms is not None:
        store_cfg["evict_cold_threshold_ms"] = evict_cold_threshold_ms

    config = {
        "common": {
            "meta_addr": meta_addr,
            "node_id": node_id,
            "scheduler_enabled": False,
            "scheduler_uds_path": "/tmp/nonexistent.sock",
            "log_dir": log_dir,
        },
        "store": store_cfg,
        "client": {
            "cache_capacity": 100000,
        },
    }
    config_path = os.path.join(ssd_dir, "falconkv_config.json")
    os.makedirs(ssd_dir, exist_ok=True)
    with open(config_path, "w") as f:
        json.dump(config, f, indent=2)
    return config_path


try:
    import torch
    from lmcache.config import LMCacheEngineMetadata
    from lmcache.v1.config import LMCacheEngineConfig
    from lmcache.v1.memory_management import PinMemoryAllocator
    from lmcache.v1.storage_backend.local_cpu_backend import LocalCPUBackend

    _HAS_LMCACHE = True
except ImportError:
    _HAS_LMCACHE = False


@pytest.fixture
def lmcache_async_loop():
    """Background thread running an asyncio event loop for LMCache tests."""
    if not _HAS_LMCACHE:
        pytest.skip("LMCache not installed")
    loop = asyncio.new_event_loop()
    thread = threading.Thread(target=loop.run_forever)
    thread.start()
    yield loop
    if loop.is_running():
        loop.call_soon_threadsafe(loop.stop)
    if thread.is_alive():
        thread.join()


@pytest.fixture
def lmcache_metadata():
    """LMCacheEngineMetadata with kv_shape=(16,2,32,4,16)."""
    if not _HAS_LMCACHE:
        pytest.skip("LMCache not installed")
    return LMCacheEngineMetadata(
        model_name="test_model",
        world_size=3,
        worker_id=123,
        fmt="vllm",
        kv_dtype=torch.bfloat16,
        kv_shape=(16, 2, 32, 4, 16),
    )


@pytest.fixture
def memory_allocator():
    """PinMemoryAllocator with 1GB capacity."""
    if not _HAS_LMCACHE:
        pytest.skip("LMCache not installed")
    allocator = PinMemoryAllocator(1024 * 1024 * 1024)
    yield allocator
    allocator.close()


@pytest.fixture
def make_falconkv_config(test_log_dir):
    """Provide _make_falconkv_config with log_dir pre-bound to test_log_dir."""
    def _make(*args, **kwargs):
        kwargs.setdefault("log_dir", test_log_dir)
        return _make_falconkv_config(*args, **kwargs)
    return _make


@pytest.fixture
def local_cpu_backend(memory_allocator, lmcache_metadata):
    """Manually constructed LocalCPUBackend for testing.

    Uses __new__ + attribute setting, consistent with the pattern in
    LMCache's own test_falcon_connector.py.
    """
    if not _HAS_LMCACHE:
        pytest.skip("LMCache not installed")
    config = LMCacheEngineConfig.from_legacy(chunk_size=32)
    metadata = lmcache_metadata
    backend = LocalCPUBackend.__new__(LocalCPUBackend)
    backend.config = config
    backend.metadata = metadata
    backend.memory_allocator = memory_allocator
    backend.dst_device = "cpu"
    return backend
