#!/usr/bin/env python3
import argparse
import asyncio
import os


def content_for_name(name: str, replica: str) -> bytes:
    object_id = name.strip("/").split("/")[-1]
    return f"{replica}:{object_id}".encode()


def _run_self_test() -> None:
    assert content_for_name("/content/object1", "producer-a") == b"producer-a:object1"
    assert content_for_name("/content/object-42", "producer-b") == b"producer-b:object-42"
    print("content_producer.py self-test passed")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--prefix", default=os.environ.get("CONTENT_PREFIX", "/content"))
    parser.add_argument("--replica", default=os.environ.get("CONTENT_REPLICA", "content"))
    parser.add_argument("--delay-ms", type=float, default=float(os.environ.get("CONTENT_DELAY_MS", "10")))
    args = parser.parse_args()

    from ndn.app import NDNApp
    from ndn.encoding import Name

    app = NDNApp()

    async def handle_interest(name, _param, _app_param):
        interest_name = Name.to_str(name)
        if args.delay_ms > 0:
            await asyncio.sleep(args.delay_ms / 1000.0)
        app.put_data(name, content=content_for_name(interest_name, args.replica),
                     freshness_period=0, digest_sha256=True)
        print(f"content={args.replica} served {interest_name}", flush=True)

    def on_interest(name, param, app_param):
        asyncio.create_task(handle_interest(name, param, app_param))

    app.route(args.prefix)(on_interest)
    print(f"content={args.replica} registering {args.prefix}", flush=True)
    app.run_forever()


if __name__ == "__main__":
    if os.environ.get("CONTENT_PRODUCER_SELF_TEST") == "1":
        _run_self_test()
    else:
        main()
