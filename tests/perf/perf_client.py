#!/usr/bin/env python3
"""Single-client performance worker for FalconKV end-to-end perf tests.

Usage:
    python perf_client.py --config perf_config.json --client-id A

This script:
1. Parses the shared perf config and extracts its own client section.
2. Generates a FalconKV JSON config file for pyfalconkv.Client.
3. Runs a warmup phase, then the main benchmark loop.
4. Records per-operation latencies (exist / put / get).
5. Computes percentile statistics and writes result_{client_id}.json.
"""

import argparse
import ctypes
import json
import math
import os
import sys
import time


# ---------------------------------------------------------------------------
# Buffer helpers (same pattern as integration tests)
# ---------------------------------------------------------------------------

class BufferGuard:
    """Holds a ctypes buffer alive and exposes its address/size."""

    def __init__(self, data: bytes):
        self._buf = (ctypes.c_ubyte * len(data))(*data)
        self.ptr = ctypes.addressof(self._buf)
        self.size = len(data)


class ZeroBuffer:
    """Allocated zeroed buffer of given size."""

    def __init__(self, size: int):
        self._buf = (ctypes.c_ubyte * size)()
        self.ptr = ctypes.addressof(self._buf)
        self.size = size


# ---------------------------------------------------------------------------
# Config helpers
# ---------------------------------------------------------------------------

def _find_client_config(config: dict, client_id: str) -> dict:
    for c in config["clients"]:
        if c["client_id"] == client_id:
            return c
    raise ValueError(f"Client '{client_id}' not found in config")


def _write_falconkv_config(client_cfg: dict, test_cfg: dict, config: dict,
                           ssd_path: str) -> str:
    """Generate a FalconKV JSON config file and return its path."""
    meta_addr = config["transfer"]["meta_addr"]
    scheduler_enabled = config["scheduler"].get("enabled", False)
    scheduler_uds = test_cfg.get("scheduler_uds_path", "/tmp/falconkv_perf_sched.sock")

    falconkv_cfg = {
        "common": {
            "meta_addr": meta_addr,
            "node_id": client_cfg["node_id"],
            "scheduler_enabled": scheduler_enabled,
            "scheduler_uds_path": scheduler_uds,
            "log_dir": ssd_path,
        },
        "store": {
            "ssd_path": ssd_path,
            "store_id": client_cfg["store_id"],
            "capacity_gb": client_cfg.get("capacity_gb", 1),
            "page_size": 4096,
            "io_threads": client_cfg.get("io_threads", 2),
            "buffer_pool_size": client_cfg.get("buffer_pool_size", 4),
            "store_rpc_host": client_cfg.get("store_rpc_host", "127.0.0.1"),
            "listen_port": client_cfg["listen_port"],
        },
        "client": {
            "cache_capacity": client_cfg.get("cache_capacity", 100000),
        },
        "meta": {
            "listen_addr": meta_addr,
        },
    }
    os.makedirs(ssd_path, exist_ok=True)
    config_path = os.path.join(ssd_path, "falconkv_config.json")
    with open(config_path, "w") as f:
        json.dump(falconkv_cfg, f, indent=2)
    return config_path


# ---------------------------------------------------------------------------
# Statistics
# ---------------------------------------------------------------------------

def _percentile(sorted_data: list, pct: float) -> float:
    """Compute percentile from sorted list."""
    if not sorted_data:
        return 0.0
    k = (len(sorted_data) - 1) * pct / 100.0
    f = math.floor(k)
    c = math.ceil(k)
    if f == c:
        return sorted_data[int(k)]
    d0 = sorted_data[int(f)] * (c - k)
    d1 = sorted_data[int(c)] * (k - f)
    return d0 + d1


def _compute_stats(latencies_ms: list, elapsed_sec: float,
                   total_bytes: int) -> dict:
    """Compute stats from a list of latencies in ms."""
    if not latencies_ms:
        return {
            "total_ops": 0,
            "avg_ms": 0.0,
            "p50_ms": 0.0,
            "p95_ms": 0.0,
            "p99_ms": 0.0,
            "min_ms": 0.0,
            "max_ms": 0.0,
            "throughput_ops": 0.0,
            "throughput_mb": 0.0,
        }
    s = sorted(latencies_ms)
    return {
        "total_ops": len(s),
        "avg_ms": round(sum(s) / len(s), 3),
        "p50_ms": round(_percentile(s, 50), 3),
        "p95_ms": round(_percentile(s, 95), 3),
        "p99_ms": round(_percentile(s, 99), 3),
        "min_ms": round(s[0], 3),
        "max_ms": round(s[-1], 3),
        "throughput_ops": round(len(s) / elapsed_sec, 1) if elapsed_sec > 0 else 0.0,
        "throughput_mb": round(total_bytes / (1024 * 1024) / elapsed_sec, 2)
        if elapsed_sec > 0 else 0.0,
    }


# ---------------------------------------------------------------------------
# Key generation
# ---------------------------------------------------------------------------

def _generate_keys(client_id: str, batch_idx: int, batch_size: int,
                   shared_ratio: float, shared_batch_idx: int = -1):
    """Generate a mix of shared and exclusive keys for one batch.

    Returns (keys, is_shared_mask) where is_shared_mask[i] is True if the key
    is a shared key written by Client A.

    shared_batch_idx controls which batch index is used for shared keys.
    When -1 (default), shared keys use batch_idx (same as exclusive keys).
    """
    keys = []
    shared_mask = []
    n_shared = max(1, int(batch_size * shared_ratio))
    n_exclusive = batch_size - n_shared

    # Use shared_batch_idx for shared keys if provided, else batch_idx
    sidx = shared_batch_idx if shared_batch_idx >= 0 else batch_idx

    # Shared keys — these are written by Client A (store_id=1, node_id=1)
    # Other clients only read them (triggering cross-store / cross-node paths)
    for i in range(n_shared):
        keys.append(f"perf_shared_{sidx}_{i}")
        shared_mask.append(True)

    # Exclusive keys — owned by this client
    for i in range(n_exclusive):
        keys.append(f"perf_{client_id}_{batch_idx}_{i}")
        shared_mask.append(False)

    return keys, shared_mask


# ---------------------------------------------------------------------------
# Main benchmark
# ---------------------------------------------------------------------------

def run_benchmark(config: dict, client_id: str):
    from pyfalconkv.client import Client

    test_cfg = config["test"]
    client_cfg = _find_client_config(config, client_id)

    ssd_path = client_cfg["ssd_path"]
    os.makedirs(ssd_path, exist_ok=True)

    # Generate FalconKV config
    config_path = _write_falconkv_config(client_cfg, test_cfg, config, ssd_path)

    # Create client
    client = Client(config_path, cache_capacity=client_cfg.get("cache_capacity", 100000))

    batch_size = test_cfg.get("batch_size", 8)
    value_size = test_cfg.get("value_size", 4096)
    shared_ratio = test_cfg.get("shared_key_ratio", 0.3)
    warmup_sec = test_cfg.get("warmup_sec", 3)
    duration_sec = test_cfg.get("duration_sec", 30)

    # ---- Warmup phase ----
    print(f"[{client_id}] Warmup: writing initial data for {warmup_sec}s ...")
    warmup_data = os.urandom(value_size)
    warmup_guard = BufferGuard(warmup_data)
    warmup_batch_count = 0
    warmup_end = time.time() + warmup_sec
    while time.time() < warmup_end:
        keys, shared_mask = _generate_keys(client_id, warmup_batch_count,
                                           batch_size, shared_ratio)
        # Only Client A writes shared keys; others just write their exclusives
        if client_id == "A":
            put_keys = keys
            put_ptrs = [warmup_guard.ptr] * len(keys)
            put_sizes = [warmup_guard.size] * len(keys)
        else:
            put_keys = [k for k, s in zip(keys, shared_mask) if not s]
            put_ptrs = [warmup_guard.ptr] * len(put_keys)
            put_sizes = [warmup_guard.size] * len(put_keys)

        if put_keys:
            client.batch_put_sync(put_keys, put_ptrs, put_sizes)
        warmup_batch_count += 1

    # Wait for meta sync across all clients
    print(f"[{client_id}] Warmup done ({warmup_batch_count} batches). "
          f"Waiting for meta sync ...")
    time.sleep(2)

    # ---- Main benchmark ----
    exist_latencies = []
    put_latencies = []
    get_latencies = []
    put_bytes = 0
    get_bytes = 0
    errors = 0
    # Reset batch_idx to 0 so that all clients reference the same shared-key
    # range written by Client A during warmup.  Using warmup_batch_count would
    # break because each client completes a different number of warmup batches
    # (Client A writes all keys; others write only exclusives), causing the
    # shared-batch offset to point to keys that were never written.
    batch_idx = 0

    print(f"[{client_id}] Running benchmark for {duration_sec}s ...")
    t_start = time.time()
    deadline = t_start + duration_sec

    while time.time() < deadline:
        # Non-A clients reference the previous batch's shared keys (written by A)
        shared_batch = batch_idx if client_id == "A" else max(0, batch_idx - 1)
        keys, shared_mask = _generate_keys(client_id, batch_idx,
                                           batch_size, shared_ratio,
                                           shared_batch_idx=shared_batch)

        # 1) batch_exist
        t0 = time.monotonic()
        try:
            client.batch_exist_sync(keys)
        except Exception as e:
            errors += 1
        t1 = time.monotonic()
        exist_latencies.append((t1 - t0) * 1000)

        # 2) batch_put — only put keys this client owns
        if client_id == "A":
            put_keys = keys
        else:
            put_keys = [k for k, s in zip(keys, shared_mask) if not s]

        if put_keys:
            write_data = os.urandom(value_size)
            write_guard = BufferGuard(write_data)
            put_ptrs = [write_guard.ptr] * len(put_keys)
            put_sizes = [write_guard.size] * len(put_keys)

            t0 = time.monotonic()
            try:
                client.batch_put_sync(put_keys, put_ptrs, put_sizes)
                put_bytes += value_size * len(put_keys)
            except Exception as e:
                errors += 1
            t1 = time.monotonic()
            put_latencies.append((t1 - t0) * 1000)

        # 3) batch_get — read all keys (triggers cross-store/cross-node for shared)
        get_buffers = []
        for k in keys:
            get_buffers.append(ZeroBuffer(value_size))

        t0 = time.monotonic()
        try:
            results = client.batch_get_sync(
                keys,
                [b.ptr for b in get_buffers],
                [b.size for b in get_buffers],
            )
            get_bytes += sum(max(0, r) for r in results)
        except Exception as e:
            errors += 1
        t1 = time.monotonic()
        get_latencies.append((t1 - t0) * 1000)

        batch_idx += 1

    elapsed = time.time() - t_start

    # Compute stats
    result = {
        "client_id": client_id,
        "node_id": client_cfg["node_id"],
        "store_id": client_cfg["store_id"],
        "elapsed_sec": round(elapsed, 2),
        "config": {
            "batch_size": batch_size,
            "value_size": value_size,
            "shared_key_ratio": shared_ratio,
        },
        "exist": _compute_stats(exist_latencies, elapsed, 0),
        "put": _compute_stats(put_latencies, elapsed, put_bytes),
        "get": _compute_stats(get_latencies, elapsed, get_bytes),
        "errors": errors,
        "total_batches": batch_idx,
    }

    # Write result file
    result_dir = test_cfg.get("result_dir", "/tmp/falconkv_perf_result")
    os.makedirs(result_dir, exist_ok=True)
    result_path = os.path.join(result_dir, f"result_{client_id}.json")
    with open(result_path, "w") as f:
        json.dump(result, f, indent=2)

    print(f"[{client_id}] Done. {result['total_batches']} batches, "
          f"{errors} errors. Results → {result_path}")

    client.close()
    return result


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------

def main():
    parser = argparse.ArgumentParser(
        description="FalconKV perf test single-client worker")
    parser.add_argument("--config", required=True,
                        help="Path to perf_config.json")
    parser.add_argument("--client-id", required=True,
                        help="Client identifier (e.g. A, B, C)")
    args = parser.parse_args()

    with open(args.config) as f:
        config = json.load(f)

    result = run_benchmark(config, args.client_id)

    # Print brief summary
    for op in ("exist", "put", "get"):
        s = result[op]
        print(f"  {op:5s}: {s['total_ops']:5d} ops  "
              f"avg={s['avg_ms']:7.3f}ms  "
              f"p99={s['p99_ms']:7.3f}ms  "
              f"ops/s={s['throughput_ops']:8.1f}")


if __name__ == "__main__":
    main()
