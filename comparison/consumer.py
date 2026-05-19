#!/usr/bin/env python3
import argparse
import asyncio
import json
import statistics
import time
from typing import List


def name_for_scenario(scenario: str, seq: int) -> str:
    object_id = f"object-{seq}"
    if scenario == "content-only":
        return f"/content/{object_id}"
    if scenario == "single-chain":
        return f"/sfc/resize/{object_id}"
    if scenario == "deep-chain":
        return f"/sfc/resize/compress/filter/{object_id}"
    raise ValueError(f"unknown scenario: {scenario}")


def percentile(values: List[float], pct: float) -> float:
    if not values:
        return 0.0
    ordered = sorted(values)
    index = int(round((pct / 100.0) * (len(ordered) - 1)))
    return ordered[index]


def summarize(scenario: str, count: int, successes: List[float], timeouts: int) -> dict:
    return {
        "scenario": scenario,
        "request_count": count,
        "success_count": len(successes),
        "timeout_count": timeouts,
        "response_avg_ms": statistics.mean(successes) if successes else 0.0,
        "response_p50_ms": percentile(successes, 50),
        "response_p95_ms": percentile(successes, 95),
        "response_max_ms": max(successes) if successes else 0.0,
    }


def _run_self_test() -> None:
    assert name_for_scenario("content-only", 7) == "/content/object-7"
    assert name_for_scenario("single-chain", 7) == "/sfc/resize/object-7"
    assert name_for_scenario("deep-chain", 7) == "/sfc/resize/compress/filter/object-7"
    assert percentile([10, 20, 30], 50) == 20
    print("consumer.py self-test passed")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--scenario", required=True, choices=["content-only", "single-chain", "deep-chain"])
    parser.add_argument("--count", type=int, default=10)
    parser.add_argument("--start-seq", type=int, default=1)
    parser.add_argument("--concurrency", type=int, default=1)
    parser.add_argument("--lifetime-ms", type=int, default=4000)
    parser.add_argument("--json", action="store_true")
    args = parser.parse_args()

    from ndn.app import NDNApp
    from ndn.types import InterestNack, InterestTimeout, ValidationFailure

    app = NDNApp()
    successes: List[float] = []
    timeout_count = 0
    sem = asyncio.Semaphore(args.concurrency)

    async def fetch_one(seq: int) -> None:
        nonlocal timeout_count
        name = name_for_scenario(args.scenario, seq)
        async with sem:
            started = time.monotonic()
            try:
                await app.express_interest(
                    name,
                    must_be_fresh=True,
                    can_be_prefix=False,
                    lifetime=args.lifetime_ms,
                )
                successes.append((time.monotonic() - started) * 1000.0)
            except (InterestNack, InterestTimeout, ValidationFailure) as exc:
                timeout_count += 1
                print(f"fetch failed {name}: {exc}", flush=True)

    async def run() -> None:
        await asyncio.gather(*(fetch_one(seq) for seq in range(args.start_seq, args.start_seq + args.count)))
        summary = summarize(args.scenario, args.count, successes, timeout_count)
        if args.json:
            print(json.dumps(summary, sort_keys=True), flush=True)
        else:
            print(summary, flush=True)
        app.shutdown()

    app.run_forever(after_start=run())


if __name__ == "__main__":
    import os
    if os.environ.get("SFC_CONSUMER_SELF_TEST") == "1":
        _run_self_test()
    else:
        main()
