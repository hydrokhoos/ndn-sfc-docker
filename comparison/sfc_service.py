#!/usr/bin/env python3
import argparse
import asyncio
import os
from typing import List


def split_name(name: str) -> List[str]:
    return [component for component in name.split("/") if component]


def next_name_for_sfc_interest(name: str, service: str) -> str:
    components = split_name(name)
    if len(components) < 3 or components[0] != "sfc":
        raise ValueError(f"not an SFC Interest name: {name}")
    if components[1] != service:
        raise ValueError(f"Interest {name} is not for service {service}")

    remaining = components[2:]
    if len(remaining) == 1:
        return f"/content/{remaining[0]}"
    return "/sfc/" + "/".join(remaining)


def _run_self_test() -> None:
    assert next_name_for_sfc_interest("/sfc/resize/compress/object1", "resize") == "/sfc/compress/object1"
    assert next_name_for_sfc_interest("/sfc/filter/object1", "filter") == "/content/object1"
    print("sfc_service.py self-test passed")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--service", default=os.environ.get("SERVICE_NAME", "resize"))
    parser.add_argument("--delay-ms", type=float, default=float(os.environ.get("SERVICE_DELAY_MS", "20")))
    parser.add_argument("--lifetime-ms", type=int, default=int(os.environ.get("INTEREST_LIFETIME_MS", "4000")))
    args = parser.parse_args()

    from ndn.app import NDNApp
    from ndn.encoding import Name
    from ndn.types import InterestNack, InterestTimeout, ValidationFailure

    app = NDNApp()
    sfc_prefix = f"/sfc/{args.service}"
    svc_prefix = f"/svc/{args.service}"

    async def handle_interest(name, _param, _app_param):
        interest_name = Name.to_str(name)
        try:
            next_name = next_name_for_sfc_interest(interest_name, args.service)
            if args.delay_ms > 0:
                await asyncio.sleep(args.delay_ms / 1000.0)

            result = await app.express_interest(
                next_name,
                must_be_fresh=True,
                can_be_prefix=False,
                lifetime=args.lifetime_ms,
            )
            _data_name, _meta, content = result[:3]
            app.put_data(name, content=bytes(content or b""), freshness_period=0, digest_sha256=True)
            print(f"service={args.service} served {interest_name} via {next_name}", flush=True)
        except (InterestNack, InterestTimeout, ValidationFailure) as exc:
            print(f"service={args.service} downstream failed {interest_name}: {exc}", flush=True)
        except Exception as exc:
            print(f"service={args.service} error {interest_name}: {exc}", flush=True)

    def on_interest(name, param, app_param):
        asyncio.create_task(handle_interest(name, param, app_param))

    app.route(sfc_prefix)(on_interest)
    app.route(svc_prefix)(on_interest)

    print(f"service={args.service} registering {sfc_prefix} and {svc_prefix}", flush=True)
    app.run_forever()


if __name__ == "__main__":
    if os.environ.get("SFC_SERVICE_SELF_TEST") == "1":
        _run_self_test()
    else:
        main()
