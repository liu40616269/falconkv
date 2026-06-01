"""
End-to-end integration tests for FalconKVConnector via LMCache RemoteBackend.

Covers three read paths:
1. ACCESS_LOCAL_DIRECT  — single connector local store read/write
2. ACCESS_NODE_DIRECT   — same node_id, different store_id cross-store read
3. ACCESS_REMOTE_RPC    — different node_id remote RPC read

Requires:
- FalconKV services (meta_server, scheduler_server) from conftest.py
- pyfalconkv with FalconKVConnector
- LMCache installed (RemoteBackend, LMCacheEngineConfig, etc.)
"""

import asyncio
import os
import time

import pytest

pytestmark = pytest.mark.integration

try:
    import torch

    from lmcache.utils import CacheEngineKey
    from lmcache.v1.config import LMCacheEngineConfig
    from lmcache.v1.memory_management import MemoryObj
    from lmcache.v1.storage_backend.remote_backend import RemoteBackend

    # Preload C extension during module import (before any event loop threads
    # are started by fixtures) to avoid thread-safety issues during C++
    # static initialization.
    from pyfalconkv import _pyfalconkv_internal  # noqa: F401
    from pyfalconkv.client import Client  # noqa: F401
    from pyfalconkv.connector import FalconKVConnector  # noqa: F401

    HAS_LMCACHE = True
except ImportError:
    HAS_LMCACHE = False


def _generate_unique_key(key_id: int = 0) -> CacheEngineKey:
    """Generate a unique CacheEngineKey using timestamp."""
    timestamp = int(time.time() * 1000000)
    return CacheEngineKey(
        fmt="vllm",
        model_name=f"test_model_{timestamp}_{key_id}",
        world_size=3,
        worker_id=123,
        chunk_hash=hash(f"{timestamp}_{key_id}"),
        dtype=torch.bfloat16,
    )


def _make_lmcache_config(
    falconkv_config_file, fire_and_forget=False, async_batch_size=16
):
    """Build an LMCacheEngineConfig pointing to falconkv://."""
    config = LMCacheEngineConfig.from_legacy(
        chunk_size=32,
        extra_config={
            "falconkv_config_file": falconkv_config_file,
            "falconkv_cache_capacity": 100000,
            "falconkv_async_batch_size": async_batch_size,
            "falconkv_fire_and_forget": fire_and_forget,
        },
    )
    config.remote_url = "falconkv://localhost:0"
    config.remote_serde = "naive"
    return config


def _wait_for_keys(backend, keys, timeout=10.0, poll=0.2):
    """Poll backend.contains() until all keys are visible."""
    t0 = time.time()
    while time.time() - t0 < timeout:
        if all(backend.contains(k) for k in keys):
            return
        time.sleep(poll)
    raise TimeoutError(f"Keys not visible after {timeout}s")


def _wait_for_data(backend, keys, timeout=15.0, poll=0.3):
    """Poll backend.get_blocking() until all keys return readable data.

    Returns the list of MemoryObj results (callers must ref_count_down).
    """
    t0 = time.time()
    while time.time() - t0 < timeout:
        results = [backend.get_blocking(k) for k in keys]
        if all(r is not None for r in results):
            return results
        for r in results:
            if r is not None:
                r.ref_count_down()
        time.sleep(poll)
    raise TimeoutError(f"Data not available after {timeout}s")


# ===========================================================================
# Test 1: ACCESS_LOCAL_DIRECT — single connector local read/write
# ===========================================================================


@pytest.mark.skipif(not HAS_LMCACHE, reason="LMCache not installed")
class TestLocalReadWrite:
    """Single connector local store read/write (ACCESS_LOCAL_DIRECT).

    Uses one RemoteBackend instance with store_id=1, node_id=1.
    """

    @pytest.fixture(autouse=True)
    def _setup_backend(
        self,
        meta_server,
        temp_ssd_dir,
        lmcache_async_loop,
        local_cpu_backend,
        lmcache_metadata,
        make_falconkv_config,
    ):
        ssd_path = os.path.join(temp_ssd_dir, "node1_store1")
        config_file = make_falconkv_config(
            ssd_dir=ssd_path,
            store_id=1,
            node_id=1,
            meta_addr=meta_server["addr"],
        )
        self.config = _make_lmcache_config(config_file, fire_and_forget=False)
        self.metadata = lmcache_metadata
        self.loop = lmcache_async_loop
        self.local_cpu_backend = local_cpu_backend
        self._make_falconkv_config = make_falconkv_config
        self._meta_addr = meta_server["addr"]

        self.backend = RemoteBackend(
            config=self.config,
            metadata=self.metadata,
            loop=self.loop,
            local_cpu_backend=self.local_cpu_backend,
            dst_device="cpu",
        )
        yield
        self.backend.close()

    def test_put_and_get_roundtrip(self):
        """Single key write -> read -> data matches."""
        key = _generate_unique_key(0)
        shapes = self.metadata.get_shapes()
        dtypes = self.metadata.get_dtypes()
        shape = shapes[0]
        dtype = dtypes[0]

        memory_obj = self.local_cpu_backend.memory_allocator.allocate(shape, dtype)
        memory_obj.raw_data.fill_(42)

        future = self.backend.submit_put_task(key, memory_obj)
        if future:
            future.result(timeout=5.0)

        assert self.backend.contains(key)

        result = self.backend.get_blocking(key)
        assert result is not None
        assert isinstance(result, MemoryObj)
        assert result.metadata.shape == memory_obj.metadata.shape
        assert result.metadata.dtype == memory_obj.metadata.dtype
        assert torch.equal(result.raw_data, memory_obj.raw_data)

        memory_obj.ref_count_down()
        result.ref_count_down()

    def test_batched_put_and_get(self):
        """Batch 5 keys -> batch read back, all match."""
        keys = [_generate_unique_key(i) for i in range(5)]
        shapes = self.metadata.get_shapes()
        dtypes = self.metadata.get_dtypes()
        shape = shapes[0]
        dtype = dtypes[0]

        memory_objs = [
            self.local_cpu_backend.memory_allocator.allocate(shape, dtype)
            for _ in range(5)
        ]
        for i, mobj in enumerate(memory_objs):
            mobj.raw_data.fill_(i + 1)

        futures = [
            self.backend.submit_put_task(key, memory_obj)
            for key, memory_obj in zip(keys, memory_objs)
        ]
        for future in filter(None, futures):
            future.result(timeout=5.0)

        for key in keys:
            assert self.backend.contains(key)

        results = self.backend.batched_get_blocking(keys)
        assert results is not None
        assert len(results) == 5
        for result, original in zip(results, memory_objs):
            assert result is not None
            assert result.metadata.shape == original.metadata.shape
            assert result.metadata.dtype == original.metadata.dtype
            assert torch.equal(result.raw_data, original.raw_data)

        for memory_obj in memory_objs:
            memory_obj.ref_count_down()
        for result in results:
            result.ref_count_down()

    def test_contains_existing_and_missing(self):
        """contains() returns True for existing keys, False for missing."""
        key = _generate_unique_key(10)
        shapes = self.metadata.get_shapes()
        dtypes = self.metadata.get_dtypes()
        shape = shapes[0]
        dtype = dtypes[0]

        assert not self.backend.contains(key)

        memory_obj = self.local_cpu_backend.memory_allocator.allocate(shape, dtype)
        future = self.backend.submit_put_task(key, memory_obj)
        if future:
            future.result(timeout=5.0)

        assert self.backend.contains(key)
        assert not self.backend.contains(_generate_unique_key(99))

        memory_obj.ref_count_down()

    def test_batched_contains_consecutive(self):
        """batched_contains() returns consecutive prefix count."""
        key_num = 176
        key_valid = 128

        keys = [_generate_unique_key(i) for i in range(key_num)]
        shapes = self.metadata.get_shapes()
        dtypes = self.metadata.get_dtypes()
        shape = shapes[0]
        dtype = dtypes[0]

        memory_objs = [
            self.local_cpu_backend.memory_allocator.allocate(shape, dtype)
            for _ in range(key_valid)
        ]

        self.backend.batched_submit_put_task(keys[:key_valid], memory_objs)
        _wait_for_keys(self.backend, keys[:key_valid])

        count = self.backend.batched_contains(keys)
        assert count == key_valid

        count_all = self.backend.batched_contains(keys[:key_valid])
        assert count_all == key_valid

        count_none = self.backend.batched_contains([keys[key_valid]])
        assert count_none == 0

        results = self.backend.batched_get_blocking(keys[:key_valid])
        for m in memory_objs:
            m.ref_count_down()
        for r in results:
            if r is not None:
                r.ref_count_down()

    def test_batched_get_non_blocking_partial(self):
        """Partial existence: batched_get_non_blocking truncates at first gap."""
        num_total = 5
        num_exist = 2
        keys = [_generate_unique_key(i) for i in range(num_total)]
        shapes = self.metadata.get_shapes()
        dtypes = self.metadata.get_dtypes()
        shape = shapes[0]
        dtype = dtypes[0]

        for i in range(num_exist):
            memory_obj = self.local_cpu_backend.memory_allocator.allocate(
                shape, dtype
            )
            memory_obj.ref_count_up()
            future = self.backend.submit_put_task(keys[i], memory_obj)
            if future:
                future.result(timeout=5.0)
            memory_obj.ref_count_down()

        # Only existing keys: should return all
        future = asyncio.run_coroutine_threadsafe(
            self.backend.batched_get_non_blocking("test_lookup", keys[:num_exist]),
            self.loop,
        )
        results = future.result(timeout=5.0)
        assert len(results) == num_exist
        for r in results:
            assert r is not None
            r.ref_count_down()

        # All 5 keys: should stop at first gap (index 2)
        future = asyncio.run_coroutine_threadsafe(
            self.backend.batched_get_non_blocking("test_lookup", keys),
            self.loop,
        )
        results = future.result(timeout=5.0)
        assert len(results) == num_exist
        for r in results:
            assert r is not None
            r.ref_count_down()

    def test_fire_and_forget_data_integrity(self):
        """Fire-and-forget mode: data written correctly after async flush."""
        # Create a new backend with fire-and-forget enabled
        ssd_path = os.path.normpath(os.path.join(
            os.path.dirname(self.config.extra_config["falconkv_config_file"]),
            "..",
            "ff_store",
        ))
        config_file = self._make_falconkv_config(
            ssd_dir=ssd_path,
            store_id=10,
            node_id=1,
            meta_addr=self._meta_addr,
        )
        ff_config = _make_lmcache_config(config_file, fire_and_forget=True)
        ff_backend = RemoteBackend(
            config=ff_config,
            metadata=self.metadata,
            loop=self.loop,
            local_cpu_backend=self.local_cpu_backend,
            dst_device="cpu",
        )

        try:
            keys = [_generate_unique_key(i) for i in range(3)]
            shapes = self.metadata.get_shapes()
            dtypes = self.metadata.get_dtypes()
            shape = shapes[0]
            dtype = dtypes[0]

            memory_objs = [
                self.local_cpu_backend.memory_allocator.allocate(shape, dtype)
                for _ in range(3)
            ]
            for i, mobj in enumerate(memory_objs):
                mobj.raw_data.fill_(i + 100)

            ff_backend.batched_submit_put_task(keys, memory_objs)
            results = _wait_for_data(ff_backend, keys)

            assert len(results) == 3
            for i, (result, original) in enumerate(
                zip(results, memory_objs, strict=True)
            ):
                assert result is not None
                assert torch.equal(result.raw_data, original.raw_data)

            for m in memory_objs:
                if m.meta.ref_count > 0:
                    m.ref_count_down()
            for r in results:
                r.ref_count_down()
        finally:
            ff_backend.close()


# ===========================================================================
# Test 2: ACCESS_NODE_DIRECT — same node, different store cross-store read
# ===========================================================================


@pytest.mark.skipif(not HAS_LMCACHE, reason="LMCache not installed")
class TestSameNodeRead:
    """Cross-store read with same node_id (ACCESS_NODE_DIRECT).

    Writer: store_id=1, node_id=1
    Reader: store_id=2, node_id=1
    Shared meta server.
    """

    @pytest.fixture(autouse=True)
    def _setup_backends(
        self,
        meta_server,
        temp_ssd_dir,
        lmcache_async_loop,
        local_cpu_backend,
        lmcache_metadata,
        make_falconkv_config,
    ):
        # Writer backend: store_id=1, node_id=1
        writer_ssd = os.path.join(temp_ssd_dir, "node1_store1")
        writer_config_file = make_falconkv_config(
            ssd_dir=writer_ssd,
            store_id=1,
            node_id=1,
            meta_addr=meta_server["addr"],
        )
        writer_config = _make_lmcache_config(writer_config_file, fire_and_forget=False)
        self.writer_backend = RemoteBackend(
            config=writer_config,
            metadata=lmcache_metadata,
            loop=lmcache_async_loop,
            local_cpu_backend=local_cpu_backend,
            dst_device="cpu",
        )

        # Reader backend: store_id=2, node_id=1
        reader_ssd = os.path.join(temp_ssd_dir, "node1_store2")
        reader_config_file = make_falconkv_config(
            ssd_dir=reader_ssd,
            store_id=2,
            node_id=1,
            meta_addr=meta_server["addr"],
        )
        reader_config = _make_lmcache_config(reader_config_file, fire_and_forget=False)
        self.reader_backend = RemoteBackend(
            config=reader_config,
            metadata=lmcache_metadata,
            loop=lmcache_async_loop,
            local_cpu_backend=local_cpu_backend,
            dst_device="cpu",
        )

        self.metadata = lmcache_metadata
        self.local_cpu_backend = local_cpu_backend
        self.loop = lmcache_async_loop

        yield
        self.writer_backend.close()
        self.reader_backend.close()

    def test_cross_store_read(self):
        """Writer(store1,node1) writes -> Reader(store2,node1) reads."""
        key = _generate_unique_key(0)
        shapes = self.metadata.get_shapes()
        dtypes = self.metadata.get_dtypes()
        shape = shapes[0]
        dtype = dtypes[0]

        memory_obj = self.local_cpu_backend.memory_allocator.allocate(shape, dtype)
        memory_obj.raw_data.fill_(77)

        # Writer puts data
        future = self.writer_backend.submit_put_task(key, memory_obj)
        if future:
            future.result(timeout=5.0)

        # Writer can see the key
        assert self.writer_backend.contains(key)

        # Reader queries meta to find the key on node_id=1
        # and reads via ACCESS_NODE_DIRECT (NodeLocalAccessor DirectIO)
        # Allow some time for meta sync
        _wait_for_keys(self.reader_backend, [key], timeout=10.0)

        result = self.reader_backend.get_blocking(key)
        assert result is not None
        assert result.metadata.shape == memory_obj.metadata.shape
        assert result.metadata.dtype == memory_obj.metadata.dtype
        assert torch.equal(result.raw_data, memory_obj.raw_data)

        memory_obj.ref_count_down()
        result.ref_count_down()

    def test_cross_store_batched_get(self):
        """Batched write -> cross-store batched read."""
        keys = [_generate_unique_key(i) for i in range(5)]
        shapes = self.metadata.get_shapes()
        dtypes = self.metadata.get_dtypes()
        shape = shapes[0]
        dtype = dtypes[0]

        memory_objs = [
            self.local_cpu_backend.memory_allocator.allocate(shape, dtype)
            for _ in range(5)
        ]
        for i, mobj in enumerate(memory_objs):
            mobj.raw_data.fill_(i + 50)

        # Writer batched put
        self.writer_backend.batched_submit_put_task(keys, memory_objs)
        _wait_for_keys(self.writer_backend, keys)

        # Reader batched get
        _wait_for_keys(self.reader_backend, keys, timeout=10.0)

        results = self.reader_backend.batched_get_blocking(keys)
        assert results is not None
        assert len(results) == 5
        for result, original in zip(results, memory_objs):
            assert result is not None
            assert result.metadata.shape == original.metadata.shape
            assert result.metadata.dtype == original.metadata.dtype
            assert torch.equal(result.raw_data, original.raw_data)

        for m in memory_objs:
            m.ref_count_down()
        for r in results:
            r.ref_count_down()


# ===========================================================================
# Test 3: ACCESS_REMOTE_RPC — different node remote RPC read
# ===========================================================================


@pytest.mark.skipif(not HAS_LMCACHE, reason="LMCache not installed")
class TestCrossNodeRead:
    """Cross-node remote RPC read (ACCESS_REMOTE_RPC).

    Writer: store_id=1, node_id=1
    Reader: store_id=3, node_id=2
    Shared meta server.

    Note: ACCESS_REMOTE_RPC requires Store RPC service to be available.
    In single-process test mode, the RPC channel may not be fully functional.
    """

    @pytest.fixture(autouse=True)
    def _setup_backends(
        self,
        meta_server,
        temp_ssd_dir,
        lmcache_async_loop,
        local_cpu_backend,
        lmcache_metadata,
        make_falconkv_config,
    ):
        # Writer backend: store_id=1, node_id=1
        writer_ssd = os.path.join(temp_ssd_dir, "node1_ssd")
        writer_config_file = make_falconkv_config(
            ssd_dir=writer_ssd,
            store_id=1,
            node_id=1,
            meta_addr=meta_server["addr"],
        )
        writer_config = _make_lmcache_config(writer_config_file, fire_and_forget=False)
        self.writer_backend = RemoteBackend(
            config=writer_config,
            metadata=lmcache_metadata,
            loop=lmcache_async_loop,
            local_cpu_backend=local_cpu_backend,
            dst_device="cpu",
        )

        # Reader backend: store_id=3, node_id=2
        reader_ssd = os.path.join(temp_ssd_dir, "node2_ssd")
        reader_config_file = make_falconkv_config(
            ssd_dir=reader_ssd,
            store_id=3,
            node_id=2,
            meta_addr=meta_server["addr"],
        )
        reader_config = _make_lmcache_config(reader_config_file, fire_and_forget=False)
        self.reader_backend = RemoteBackend(
            config=reader_config,
            metadata=lmcache_metadata,
            loop=lmcache_async_loop,
            local_cpu_backend=local_cpu_backend,
            dst_device="cpu",
        )

        self.metadata = lmcache_metadata
        self.local_cpu_backend = local_cpu_backend
        self.loop = lmcache_async_loop

        yield
        self.writer_backend.close()
        self.reader_backend.close()

    def test_remote_read(self):
        """Writer(node1) writes -> Reader(node2) cross-node RPC read."""
        key = _generate_unique_key(0)
        shapes = self.metadata.get_shapes()
        dtypes = self.metadata.get_dtypes()
        shape = shapes[0]
        dtype = dtypes[0]

        memory_obj = self.local_cpu_backend.memory_allocator.allocate(shape, dtype)
        memory_obj.raw_data.fill_(99)

        # Writer puts data
        future = self.writer_backend.submit_put_task(key, memory_obj)
        if future:
            future.result(timeout=5.0)

        assert self.writer_backend.contains(key)

        # Reader must discover key on node1 via Meta, then use
        # StoreRpcClient to fetch data via ACCESS_REMOTE_RPC
        _wait_for_keys(self.reader_backend, [key], timeout=10.0)

        result = self.reader_backend.get_blocking(key)
        assert result is not None
        assert result.metadata.shape == memory_obj.metadata.shape
        assert result.metadata.dtype == memory_obj.metadata.dtype
        assert torch.equal(result.raw_data, memory_obj.raw_data)

        memory_obj.ref_count_down()
        result.ref_count_down()

    def test_remote_batched_get(self):
        """Batched cross-node RPC read."""
        keys = [_generate_unique_key(i) for i in range(3)]
        shapes = self.metadata.get_shapes()
        dtypes = self.metadata.get_dtypes()
        shape = shapes[0]
        dtype = dtypes[0]

        memory_objs = [
            self.local_cpu_backend.memory_allocator.allocate(shape, dtype)
            for _ in range(3)
        ]
        for i, mobj in enumerate(memory_objs):
            mobj.raw_data.fill_(i + 200)

        self.writer_backend.batched_submit_put_task(keys, memory_objs)
        _wait_for_keys(self.writer_backend, keys)

        _wait_for_keys(self.reader_backend, keys, timeout=10.0)

        results = self.reader_backend.batched_get_blocking(keys)
        assert results is not None
        assert len(results) == 3
        for result, original in zip(results, memory_objs):
            assert result is not None
            assert torch.equal(result.raw_data, original.raw_data)

        for m in memory_objs:
            m.ref_count_down()
        for r in results:
            r.ref_count_down()
