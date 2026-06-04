#!/usr/bin/env python3
"""Single-client performance worker for FalconKV end-to-end perf tests.

Usage:
    python perf_client.py --config perf_config.json --client-id A

Architecture:
    - Client A: writer + local reader   (ACCESS_LOCAL_DIRECT)
    - Client B: same-node reader         (ACCESS_NODE_DIRECT)
    - Client C: cross-node reader         (ACCESS_REMOTE_RPC)

Key design:
    - Client A is the sole writer. batch_idx increments continuously,
      producing unlimited new keys (perf_A_{batch_idx}_{i}).
    - Clients B/C only read, referencing A's previously written keys
      (lag by 1 batch to ensure data has been synced to Meta).
    - Before every write, batch_exist checks whether keys already exist
      to avoid duplicate writes.
    - capacity_gb in the config should be large enough to hold all data
      written during warmup + benchmark (evict optimization pending).
"""

import argparse
import ctypes
import json
import math
import os
import sys
import time


# ---------------------------------------------------------------------------
# Buffer helpers
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
    scheduler_uds = test_cfg.get("scheduler_uds_path",
                                  "/tmp/falconkv_perf_sched.sock")

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
            "capacity_gb": client_cfg.get("capacity_gb", 8),
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
# Key generation
# ---------------------------------------------------------------------------

def _generate_keys(role: str, client_id: str, batch_idx: int,
                   batch_size: int) -> list:
    """Generate keys for one batch iteration.

    - Writer (A): produces fresh keys with the current batch_idx.
    - Readers (B/C): reference A's *previous* batch to ensure the data
      has already been written and synced to Meta.
    """
    keys = []
    if role == "writer":
        for i in range(batch_size):
            keys.append(f"perf_A_{batch_idx}_{i}")
    else:
        # Lag behind A by 1 batch so the data is guaranteed to exist.
        read_batch = max(0, batch_idx - 1)
        for i in range(batch_size):
            keys.append(f"perf_A_{read_batch}_{i}")
    return keys


# ---------------------------------------------------------------------------
# Statistics
# ---------------------------------------------------------------------------

def _percentile(sorted_data: list, pct: float) -> float:
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
        "throughput_ops": round(len(s) / elapsed_sec, 1)
        if elapsed_sec > 0 else 0.0,
        "throughput_mb": round(total_bytes / (1024 * 1024) / elapsed_sec, 2)
        if elapsed_sec > 0 else 0.0,
    }


# ---------------------------------------------------------------------------
# Determine client role
# ---------------------------------------------------------------------------

def _get_role(client_id: str, client_cfg: dict, config: dict) -> str:
    """Return the benchmark role for this client."""
    if client_id == "A":
        return "writer"
    writer_cfg = _find_client_config(config, "A")
    if client_cfg["node_id"] == writer_cfg["node_id"]:
        return "same_node_reader"
    return "cross_node_reader"


# ---------------------------------------------------------------------------
# Main benchmark
# ---------------------------------------------------------------------------

def run_benchmark(config: dict, client_id: str):
    from pyfalconkv.client import Client

    test_cfg = config["test"]
    client_cfg = _find_client_config(config, client_id)

    ssd_path = client_cfg["ssd_path"]
    os.makedirs(ssd_path, exist_ok=True)

    config_path = _write_falconkv_config(client_cfg, test_cfg, config, ssd_path)
    client = Client(config_path,
                    cache_capacity=client_cfg.get("cache_capacity", 100000))

    batch_size = test_cfg.get("batch_size", 16)
    value_size = test_cfg.get("value_size", 4096)
    warmup_sec = test_cfg.get("warmup_sec", 10)
    duration_sec = test_cfg.get("duration_sec", 30)
    capacity_gb = client_cfg.get("capacity_gb", 8)

    role = _get_role(client_id, client_cfg, config)

    print(f"[{client_id}] role={role}  batch_size={batch_size}  "
          f"value_size={value_size}  capacity_gb={capacity_gb}")

    # ---- Warmup phase -------------------------------------------------------
    if role == "writer":
        # Client A writes continuously to populate data for B/C to read.
        print(f"[A] Warmup: writing initial data for {warmup_sec}s ...")
        warmup_data = os.urandom(value_size)
        warmup_guard = BufferGuard(warmup_data)
        warmup_batch_count = 0
        warmup_end = time.time() + warmup_sec

        while time.time() < warmup_end:
            keys = _generate_keys(role, client_id,
                                  warmup_batch_count, batch_size)
            # Check existence before writing to avoid duplicates
            hit_count = client.batch_exist_sync(keys)
            if hit_count < len(keys):
                client.batch_put_sync(
                    keys,
                    [warmup_guard.ptr] * len(keys),
                    [warmup_guard.size] * len(keys),
                )
            warmup_batch_count += 1

        print(f"[A] Warmup done ({warmup_batch_count} batches).")
    else:
        # B / C simply wait for A to finish populating data.
        print(f"[{client_id}] Warmup: waiting {warmup_sec}s "
              f"for Client A to write data ...")
        time.sleep(warmup_sec)

    # Allow meta sync to propagate across clients.
    print(f"[{client_id}] Waiting for meta sync ...")
    time.sleep(2)

    # ---- Main benchmark -----------------------------------------------------
    exist_latencies = []
    put_latencies = []
    get_latencies = []
    put_bytes = 0
    get_bytes = 0
    errors = 0
    put_skip_count = 0   # batches where all keys already existed
    put_exec_count = 0   # batches where put was actually called
    get_hit_count = 0    # keys successfully read
    get_miss_count = 0   # keys not found / failed

    # Reset batch_idx to 0 so that all readers reference the same key
    # range written by Client A during warmup.  Using warmup_batch_count
    # would break because each client has a different warmup trajectory.
    batch_idx = 0

    print(f"[{client_id}] Running benchmark for {duration_sec}s ...")
    t_start = time.time()
    deadline = t_start + duration_sec

    while time.time() < deadline:
        keys = _generate_keys(role, client_id, batch_idx, batch_size)

        # 1) batch_exist — all clients
        t0 = time.monotonic()
        try:
            hit_count = client.batch_exist_sync(keys)
        except Exception:
            errors += 1
            hit_count = 0
        t1 = time.monotonic()
        exist_latencies.append((t1 - t0) * 1000)

        # 2) batch_put — Client A only, conditional on existence
        if role == "writer":
            if hit_count < len(keys):
                # At least one key is new — attempt to write.
                # The store internally skips keys that already exist, so
                # it is safe to pass the full batch.
                write_data = os.urandom(value_size)
                write_guard = BufferGuard(write_data)

                t0 = time.monotonic()
                try:
                    client.batch_put_sync(
                        keys,
                        [write_guard.ptr] * len(keys),
                        [write_guard.size] * len(keys),
                    )
                    put_bytes += value_size * (len(keys) - hit_count)
                    put_exec_count += 1
                except Exception:
                    errors += 1
                t1 = time.monotonic()
                put_latencies.append((t1 - t0) * 1000)
            else:
                put_skip_count += 1

        # 3) batch_get — all clients read the same keys
        get_buffers = [ZeroBuffer(value_size) for _ in keys]

        t0 = time.monotonic()
        try:
            results = client.batch_get_sync(
                keys,
                [b.ptr for b in get_buffers],
                [b.size for b in get_buffers],
            )
            for r in results:
                if r > 0:
                    get_bytes += r
                    get_hit_count += 1
                else:
                    get_miss_count += 1
        except Exception:
            errors += 1
        t1 = time.monotonic()
        get_latencies.append((t1 - t0) * 1000)

        batch_idx += 1

    elapsed = time.time() - t_start

    # ---- Compute statistics --------------------------------------------------
    result = {
        "client_id": client_id,
        "role": role,
        "node_id": client_cfg["node_id"],
        "store_id": client_cfg["store_id"],
        "elapsed_sec": round(elapsed, 2),
        "config": {
            "batch_size": batch_size,
            "value_size": value_size,
        },
        "exist": _compute_stats(exist_latencies, elapsed, 0),
        "put": _compute_stats(put_latencies, elapsed, put_bytes),
        "get": _compute_stats(get_latencies, elapsed, get_bytes),
        "put_exec_count": put_exec_count,
        "put_skip_count": put_skip_count,
        "get_hit_count": get_hit_count,
        "get_miss_count": get_miss_count,
        "errors": errors,
        "total_batches": batch_idx,
    }

    result_dir = test_cfg.get("result_dir", "/tmp/falconkv_perf_result")
    os.makedirs(result_dir, exist_ok=True)
    result_path = os.path.join(result_dir, f"result_{client_id}.json")
    with open(result_path, "w") as f:
        json.dump(result, f, indent=2)

    print(f"[{client_id}] Done. {result['total_batches']} batches, "
          f"{errors} errors. Results -> {result_path}")
    if role == "writer":
        print(f"[A] put_exec={put_exec_count}  put_skip={put_skip_count}")
    print(f"[{client_id}] get_hit={get_hit_count}  get_miss={get_miss_count}")

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

    for op in ("exist", "put", "get"):
        s = result[op]
        print(f"  {op:5s}: {s['total_ops']:5d} ops  "
              f"avg={s['avg_ms']:7.3f}ms  "
              f"p99={s['p99_ms']:7.3f}ms  "
              f"ops/s={s['throughput_ops']:8.1f}")


if __name__ == "__main__":
    main()
