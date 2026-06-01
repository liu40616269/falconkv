"""
End-to-end functional integration tests for FalconKVConnector interface.

Uses pyfalconkv.Client (→ FalconKVBridge → FalconKVClientImpl → FalconKVStore)
against a local SSD store to verify real data read/write correctness.
No external services (Meta, Scheduler) required.
"""

import ctypes
import json
import os
import shutil
import tempfile

import pytest

pytestmark = pytest.mark.integration

# ---------------------------------------------------------------------------
# Helpers — buffer management with explicit lifetime control
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


def _make_client(ssd_dir, store_id=1, capacity_gb=1, chunk_size=4096):
    """Create a pyfalconkv.Client backed by a temporary local SSD store."""
    from pyfalconkv.client import Client

    config = {
        "store": {
            "ssd_path": ssd_dir,
            "store_id": store_id,
            "node_id": 1,
            "capacity_gb": capacity_gb,
            "chunk_size": chunk_size,
            "page_size": chunk_size,
            "io_threads": 2,
            "buffer_pool_size": 4,
            "scheduler_enabled": False,
        },
        "client": {
            "cache_capacity": 10000,
            "scheduler_enabled": False,
        },
        "meta": {"listen_addr": "0.0.0.0:19999"},
        "scheduler": {"uds_path": "/tmp/nonexistent.sock", "enabled": False},
        "transfer": {"meta_addr": "localhost:19999"},
    }
    config_path = os.path.join(ssd_dir, "config.json")
    os.makedirs(ssd_dir, exist_ok=True)
    with open(config_path, "w") as f:
        json.dump(config, f)

    return Client(config_path, cache_capacity=10000)


# ---------------------------------------------------------------------------
# Fixture
# ---------------------------------------------------------------------------

@pytest.fixture
def ssd_dir():
    d = tempfile.mkdtemp(prefix="falconkv_e2e_")
    yield d
    shutil.rmtree(d, ignore_errors=True)


@pytest.fixture
def client(ssd_dir):
    if not _can_import_client():
        pytest.skip("pyfalconkv.Client not available")
    c = _make_client(ssd_dir)
    yield c
    c.close()


# ===========================================================================
# Test classes
# ===========================================================================

@pytest.mark.skipif(not _can_import_client(), reason="pyfalconkv not available")
class TestBatchPutGet:
    """Single-key and multi-key batch put/get correctness."""

    def test_single_key_roundtrip(self, client):
        """BatchPut one key → BatchGet → byte-exact match."""
        data = b"Hello FalconKV! " * 256  # 4096 bytes (chunk-aligned)
        w = BufferGuard(data)

        client.batch_put_sync(["k1"], [w.ptr], [w.size])

        r = ZeroBuffer(w.size)
        results = client.batch_get_sync(["k1"], [r.ptr], [r.size])

        assert len(results) == 1
        assert results[0] == w.size
        assert r.read_bytes() == data

    def test_multiple_keys(self, client):
        """BatchPut N keys → BatchGet → all match."""
        n = 10
        keys = []
        ptrs = []
        sizes = []
        guards = []
        expected = {}

        for i in range(n):
            key = f"multi_{i:03d}"
            data = bytes([i % 256]) * 4096
            w = BufferGuard(data)
            keys.append(key)
            ptrs.append(w.ptr)
            sizes.append(w.size)
            guards.append(w)
            expected[key] = data

        client.batch_put_sync(keys, ptrs, sizes)

        # Read back each key individually.
        for key, data in expected.items():
            r = ZeroBuffer(len(data))
            results = client.batch_get_sync([key], [r.ptr], [r.size])
            assert results[0] == len(data), f"Get failed for {key}"
            assert r.read_bytes() == data, f"Data mismatch for {key}"

    def test_overwrite_key(self, client):
        """Put same key twice → second value wins."""
        w1 = BufferGuard(b"AAAA" * 1024)
        w2 = BufferGuard(b"BBBB" * 1024)

        client.batch_put_sync(["k"], [w1.ptr], [w1.size])
        client.batch_put_sync(["k"], [w2.ptr], [w2.size])

        r = ZeroBuffer(w2.size)
        results = client.batch_get_sync(["k"], [r.ptr], [r.size])
        assert results[0] == w2.size
        assert r.read_bytes() == b"BBBB" * 1024

    def test_get_nonexistent_key(self, client):
        """Get a key that was never put → returns <=0."""
        r = ZeroBuffer(4096)
        results = client.batch_get_sync(["no_such_key"], [r.ptr], [r.size])
        assert results[0] <= 0

    def test_large_value(self, client):
        """Put/Get a value larger than one chunk (64 KB)."""
        data = os.urandom(65536)
        w = BufferGuard(data)

        client.batch_put_sync(["large"], [w.ptr], [w.size])

        r = ZeroBuffer(w.size)
        results = client.batch_get_sync(["large"], [r.ptr], [r.size])
        assert results[0] == w.size
        assert r.read_bytes() == data


@pytest.mark.skipif(not _can_import_client(), reason="pyfalconkv not available")
class TestBatchExist:
    """BatchExist correctness after Put."""

    def test_exist_after_put(self, client):
        """BatchExist returns > 0 after BatchPut."""
        w = BufferGuard(b"X" * 4096)
        client.batch_put_sync(["exist1"], [w.ptr], [w.size])

        hit = client.batch_exist_sync(["exist1"])
        assert hit >= 1

    def test_exist_miss(self, client):
        """BatchExist returns 0 for nonexistent keys."""
        hit = client.batch_exist_sync(["never_put"])
        assert hit == 0

    def test_exist_mixed(self, client):
        """BatchExist with mix of existing and missing keys."""
        w = BufferGuard(b"Y" * 4096)
        keys = ["mix_0", "mix_1", "mix_2"]
        client.batch_put_sync(keys, [w.ptr] * 3, [w.size] * 3)

        query = ["mix_0", "ghost", "mix_2"]
        hit = client.batch_exist_sync(query)
        assert hit == 2  # mix_0 and mix_2 exist


@pytest.mark.skipif(not _can_import_client(), reason="pyfalconkv not available")
class TestSequentialWrites:
    """Sequential write → read consistency over time."""

    def test_write_read_100_keys(self, client):
        """Write 100 keys sequentially, read all back."""
        n = 100
        guards = []
        for i in range(n):
            key = f"seq_{i:04d}"
            data = bytes([i % 256]) * 4096
            w = BufferGuard(data)
            guards.append(w)
            client.batch_put_sync([key], [w.ptr], [w.size])

        # Read all back.
        errors = 0
        for i in range(n):
            key = f"seq_{i:04d}"
            r = ZeroBuffer(4096)
            results = client.batch_get_sync([key], [r.ptr], [r.size])
            if results[0] != 4096:
                errors += 1
                continue
            if r.read_bytes() != bytes([i % 256]) * 4096:
                errors += 1

        assert errors == 0, f"{errors}/100 keys had mismatch"

    def test_latest_write_wins(self, client):
        """Repeated writes to the same key; each read sees the latest."""
        for version in range(5):
            data = bytes([version]) * 4096
            w = BufferGuard(data)
            client.batch_put_sync(["ver_key"], [w.ptr], [w.size])

            r = ZeroBuffer(4096)
            results = client.batch_get_sync(["ver_key"], [r.ptr], [r.size])
            assert results[0] == 4096
            assert r.read_bytes() == data, f"Version {version} mismatch"


@pytest.mark.skipif(not _can_import_client(), reason="pyfalconkv not available")
class TestClientLifecycle:
    """Client create/close/recreate lifecycle."""

    def test_create_close_recreate_fresh(self, ssd_dir):
        """Client can be created, closed, and recreated on the same SSD path.

        Note: Local FalconKVStore keeps metadata in memory. After close(),
        the in-memory index is lost and SSD data becomes unreachable until
        Meta resync restores it. This test verifies the lifecycle works
        without crash — the new client starts with a fresh index.
        """
        data = b"persist" * 256  # 4096 bytes, chunk-aligned

        c1 = _make_client(ssd_dir)
        w = BufferGuard(data)
        c1.batch_put_sync(["persist_key"], [w.ptr], [w.size])

        # Verify data
        c1.close()

        # Recreate — this starts a fresh store with empty in-memory index.
        c2 = _make_client(ssd_dir)

        # New client can write and read new data.
        w2 = BufferGuard(b"fresh_data_after_restart")
        c2.batch_put_sync(["new_key"], [w2.ptr], [w2.size])
        r2 = ZeroBuffer(w2.size)
        results2 = c2.batch_get_sync(["new_key"], [r2.ptr], [r2.size])
        assert results2[0] == w2.size
        assert r2.read_bytes() == b"fresh_data_after_restart"
        c2.close()

    def test_batch_get_empty_keys(self, client):
        """BatchGet with empty key list returns empty result."""
        results = client.batch_get_sync([], [], [])
        assert results == []


@pytest.mark.skipif(not _can_import_client(), reason="pyfalconkv not available")
class TestDifferentValueSizes:
    """Test various data sizes."""

    @pytest.mark.parametrize("size", [512, 1024, 4096, 8192])
    def test_various_sizes(self, client, size):
        """Put/Get with different data sizes."""
        data = os.urandom(size)
        w = BufferGuard(data)
        client.batch_put_sync([f"s{size}"], [w.ptr], [w.size])

        r = ZeroBuffer(size)
        results = client.batch_get_sync([f"s{size}"], [r.ptr], [r.size])
        assert results[0] == size
        assert r.read_bytes() == data
