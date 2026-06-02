#!/usr/bin/env python3
"""Aggregate perf test results from all clients and print summary.

Usage:
    python perf_aggregate.py --result-dir /tmp/falconkv_perf_result --client-ids "A B C"

Reads each client's result_{id}.json, prints a summary table, and writes
summary.json to the result directory.
"""

import argparse
import json
import os
import sys


def _load_result(result_dir: str, client_id: str) -> dict:
    path = os.path.join(result_dir, f"result_{client_id}.json")
    with open(path) as f:
        return json.load(f)


def _fmt(val: float, width: int = 8) -> str:
    """Format a float to fit in a column."""
    if val == 0:
        return "0".rjust(width)
    if val < 1:
        return f"{val:.3f}".rjust(width)
    if val < 1000:
        return f"{val:.1f}".rjust(width)
    return f"{val:.0f}".rjust(width)


def main():
    parser = argparse.ArgumentParser(
        description="FalconKV perf test result aggregator")
    parser.add_argument("--result-dir", required=True,
                        help="Directory containing result_*.json files")
    parser.add_argument("--client-ids", required=True,
                        help="Space-separated client IDs, e.g. 'A B C'")
    args = parser.parse_args()

    client_ids = args.client_ids.split()
    results = {}
    for cid in client_ids:
        try:
            results[cid] = _load_result(args.result_dir, cid)
        except FileNotFoundError:
            print(f"WARNING: result file for client '{cid}' not found, skipping")

    if not results:
        print("ERROR: No result files found.")
        sys.exit(1)

    # Print summary table
    sep = "-" * 100
    print()
    print("=" * 100)
    print("  FalconKV Performance Summary")
    print("=" * 100)
    print(f"  {'Client':<7} | {'Op':<6} | {'Total':>6} | {'Avg(ms)':>8} | "
          f"{'P50(ms)':>8} | {'P95(ms)':>8} | {'P99(ms)':>8} | "
          f"{'Ops/s':>8} | {'MB/s':>6} | {'Errors':>6}")
    print(f"  {sep}")

    summary_rows = []

    for cid in client_ids:
        if cid not in results:
            continue
        r = results[cid]
        node_id = r["node_id"]
        store_id = r["store_id"]

        for op in ("exist", "put", "get"):
            s = r[op]
            label = f"{cid}(n{node_id},s{store_id})"
            print(f"  {label:<7} | {op:<6} | {s['total_ops']:>6} | "
                  f"{_fmt(s['avg_ms']):>8} | {_fmt(s['p50_ms']):>8} | "
                  f"{_fmt(s['p95_ms']):>8} | {_fmt(s['p99_ms']):>8} | "
                  f"{_fmt(s['throughput_ops']):>8} | "
                  f"{_fmt(s['throughput_mb']):>6} | "
                  f"{r['errors']:>6}")
            summary_rows.append({
                "client_id": cid,
                "node_id": node_id,
                "store_id": store_id,
                "op": op,
                **s,
            })

    print("=" * 100)

    # Aggregate totals per operation
    print()
    print("  Aggregate (all clients):")
    print(f"  {'Op':<6} | {'Total':>7} | {'Avg(ms)':>8} | {'P99(ms)':>8} | "
          f"{'Ops/s':>8} | {'MB/s':>6}")
    print(f"  {sep}")

    for op in ("exist", "put", "get"):
        op_rows = [row for row in summary_rows if row["op"] == op]
        if not op_rows:
            continue
        total_ops = sum(r["total_ops"] for r in op_rows)
        # Weighted average
        if total_ops > 0:
            avg = sum(r["avg_ms"] * r["total_ops"] for r in op_rows) / total_ops
        else:
            avg = 0.0
        total_throughput = sum(r["throughput_ops"] for r in op_rows)
        total_mb = sum(r["throughput_mb"] for r in op_rows)
        # Use max p99 across clients
        p99 = max(r["p99_ms"] for r in op_rows)

        print(f"  {op:<6} | {total_ops:>7} | {_fmt(avg):>8} | "
              f"{_fmt(p99):>8} | {_fmt(total_throughput):>8} | "
              f"{_fmt(total_mb):>6}")

    print(f"  {sep}")
    print()

    # Write summary.json
    summary_path = os.path.join(args.result_dir, "summary.json")
    summary = {
        "per_client": {cid: results[cid] for cid in results},
        "rows": summary_rows,
    }
    with open(summary_path, "w") as f:
        json.dump(summary, f, indent=2)
    print(f"  Summary written to {summary_path}")


if __name__ == "__main__":
    main()
