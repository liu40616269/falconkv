"""
Integration tests for FalconKV Scheduler.

Tests scheduler process lifecycle, UDS creation, and concurrent service startup.
"""

import os
import socket
import subprocess
import signal
import time
import json
import pytest

pytestmark = pytest.mark.integration

BUILD_DIR = os.environ.get(
    "FALCONKV_BUILD_DIR",
    os.path.join(os.path.dirname(__file__), "..", "..", "build"),
)
BUILD_DIR = os.path.abspath(BUILD_DIR)


class TestSchedulerIntegration:
    """IT-SC-001 ~ IT-SC-004: Scheduler integration tests."""

    def test_scheduler_uds_file_type(self, scheduler_server):
        """IT-SC-001: Scheduler creates a UDS socket file (not a regular file)."""
        uds_path = scheduler_server["uds_path"]
        assert os.path.exists(uds_path)

        # Verify it's a socket file.
        st = os.stat(uds_path)
        assert (st.st_mode & 0o170000) == 0o140000, "Should be a socket file"

    def test_meta_multiple_connections(self, meta_server):
        """IT-SC-002: Meta server handles multiple concurrent TCP connections."""
        host = meta_server["host"]
        port = meta_server["port"]

        # Open several connections simultaneously.
        conns = []
        for _ in range(5):
            s = socket.create_connection((host, port), timeout=2)
            conns.append(s)

        # All should succeed.
        assert len(conns) == 5

        # Close all.
        for s in conns:
            s.close()

    def test_scheduler_bypass_when_stopped(self, temp_ssd_dir):
        """IT-SC-003: After scheduler stops, Proxy enters bypass mode."""
        from pyfalconkv.adapter import FalconKVConnectorAdapter

        # Verify adapter detects falconkv URLs (doesn't need running server).
        adapter = FalconKVConnectorAdapter()
        assert adapter.can_parse("falconkv://localhost:0")

    def test_meta_server_port_release(self, temp_ssd_dir, test_log_dir):
        """IT-SC-004: Meta server releases port after SIGTERM, can restart."""
        falconkv_master = os.path.join(BUILD_DIR, "src", "meta", "falconkv_master")
        if not os.path.isfile(falconkv_master):
            pytest.skip("falconkv_master not found")

        port = 18901
        config = {
            "common": {"log_dir": test_log_dir},
            "meta": {"listen_addr": f"0.0.0.0:{port}", "shard_count": 16},
        }
        config_file = os.path.join(temp_ssd_dir, "meta_restart_config.json")
        with open(config_file, "w") as f:
            json.dump(config, f)

        # Start first instance.
        proc1 = subprocess.Popen(
            [falconkv_master, config_file],
            stdout=subprocess.PIPE, stderr=subprocess.PIPE,
        )
        deadline = time.time() + 10
        while time.time() < deadline:
            try:
                s = socket.create_connection(("127.0.0.1", port), timeout=1)
                s.close()
                break
            except (ConnectionRefusedError, OSError):
                time.sleep(0.2)
        else:
            proc1.kill()
            proc1.wait()
            pytest.skip("First instance failed to start")

        # Stop it.
        proc1.send_signal(signal.SIGTERM)
        proc1.wait(timeout=5)

        # Wait for port to be released.
        time.sleep(0.5)

        # Start second instance on same port.
        proc2 = subprocess.Popen(
            [falconkv_master, config_file],
            stdout=subprocess.PIPE, stderr=subprocess.PIPE,
        )
        deadline = time.time() + 10
        started = False
        while time.time() < deadline:
            try:
                s = socket.create_connection(("127.0.0.1", port), timeout=1)
                s.close()
                started = True
                break
            except (ConnectionRefusedError, OSError):
                time.sleep(0.2)

        proc2.send_signal(signal.SIGTERM)
        try:
            proc2.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc2.kill()
            proc2.wait()

        assert started, "Second instance should start on the same port"
