#!/usr/bin/env python3
import asyncio
import os
from dataclasses import dataclass
from typing import Dict, List, Sequence, Tuple


RELAY_PREFIX = os.environ.get("RELAY_PREFIX", "/relay")
RELAY_UPSTREAM_PREFIX = os.environ.get("RELAY_UPSTREAM_PREFIX", "")
SEGMENT_SIZE = int(os.environ.get("RELAY_SEGMENT_SIZE", "4096"))
FRESHNESS_PERIOD = int(os.environ.get("RELAY_FRESHNESS_MS", "10000"))
SEGMENT_COMPONENT_TYPE = 0x32


def convert_relay_name(
    name: str,
    relay_prefix: str = RELAY_PREFIX,
    upstream_prefix: str = RELAY_UPSTREAM_PREFIX,
) -> str:
    """Convert /relay/a/b/c to /a/b/c, optionally under an upstream prefix."""
    normalized_name = "/" + name.strip("/")
    normalized_prefix = "/" + relay_prefix.strip("/")

    if normalized_name == normalized_prefix:
        raise ValueError("relay Interest must include a name after the relay prefix")
    if not normalized_name.startswith(normalized_prefix + "/"):
        raise ValueError(f"Interest {normalized_name} is outside relay prefix {normalized_prefix}")

    suffix = normalized_name[len(normalized_prefix):]
    if not upstream_prefix:
        return suffix or "/"
    return "/" + "/".join(part.strip("/") for part in (upstream_prefix, suffix) if part.strip("/"))


def strip_relay_prefix(name: str, relay_prefix: str = RELAY_PREFIX) -> str:
    return convert_relay_name(name, relay_prefix, "")


def process_content(data: bytes) -> bytes:
    return data


def chunk_bytes(data: bytes, segment_size: int = SEGMENT_SIZE) -> List[bytes]:
    if segment_size <= 0:
        raise ValueError("segment size must be positive")
    return [data[i:i + segment_size] for i in range(0, len(data), segment_size)] or [b""]


def _run_self_test() -> None:
    assert strip_relay_prefix("/relay/sample.txt") == "/sample.txt"
    assert strip_relay_prefix("/relay/a/b/c") == "/a/b/c"
    assert convert_relay_name("/relay1/sample.txt", "/relay1", "/producer1") == "/producer1/sample.txt"
    assert chunk_bytes(b"abcdef", 2) == [b"ab", b"cd", b"ef"]
    print("relay.py self-test passed")


@dataclass
class CachedObject:
    producer_name: str
    relay_base_name: object
    segments: List[bytes]
    final_segment: object


def run_relay() -> None:
    from ndn.app import NDNApp
    from ndn.encoding import Component, InterestParam, Name
    from ndn.types import InterestNack, InterestTimeout, ValidationFailure

    app = NDNApp()
    cache: Dict[Tuple[str, ...], CachedObject] = {}

    def is_segment_component(component: bytes) -> bool:
        is_segment = getattr(Component, "is_segment", None)
        if is_segment is not None:
            return bool(is_segment(component))

        get_type = getattr(Component, "get_type", None)
        if get_type is not None:
            return get_type(component) == getattr(Component, "TYPE_SEGMENT", SEGMENT_COMPONENT_TYPE)

        return bool(component) and component[0] == SEGMENT_COMPONENT_TYPE

    def is_version_component(component: bytes) -> bool:
        get_type = getattr(Component, "get_type", None)
        if get_type is None:
            return False
        return get_type(component) == getattr(Component, "TYPE_VERSION", 0x36)

    def remove_chunk_suffix(name: Sequence[bytes]) -> Sequence[bytes]:
        if name and is_segment_component(name[-1]):
            name = name[:-1]
            if name and is_version_component(name[-1]):
                name = name[:-1]
        return name

    def segment_number(name: Sequence[bytes]) -> int:
        if name and is_segment_component(name[-1]):
            return Component.to_number(name[-1])
        return 0

    async def fetch_producer_content(producer_name: str) -> bytes:
        first_result = await app.express_interest(
            producer_name,
            must_be_fresh=True,
            can_be_prefix=True,
            lifetime=4000,
        )
        first_name, first_meta, first_content = first_result[:3]

        versioned_name = first_name[:-1] if is_segment_component(first_name[-1]) else first_name
        chunks = [bytes(first_content or b"")]
        final_block_id = first_meta.final_block_id

        if final_block_id is not None and is_segment_component(final_block_id):
            final_segment = Component.to_number(final_block_id)
            for seg_no in range(1, final_segment + 1):
                seg_name = versioned_name + [Component.from_segment(seg_no)]
                result = await app.express_interest(
                    seg_name,
                    must_be_fresh=False,
                    can_be_prefix=False,
                    lifetime=4000,
                )
                _, _, content = result[:3]
                chunks.append(bytes(content or b""))

        return b"".join(chunks)

    async def prepare_object(relay_base_name: Sequence[bytes]) -> CachedObject:
        key = tuple(relay_base_name)
        cached = cache.get(key)
        if cached is not None:
            return cached

        producer_name = convert_relay_name(Name.to_str(relay_base_name))
        print(f"Fetching {producer_name} for {Name.to_str(relay_base_name)}", flush=True)
        producer_content = await fetch_producer_content(producer_name)
        processed = process_content(producer_content)
        segments = chunk_bytes(processed)
        final_segment = Component.from_segment(len(segments) - 1)
        cached = CachedObject(
            producer_name=producer_name,
            relay_base_name=list(relay_base_name),
            segments=segments,
            final_segment=final_segment,
        )
        cache[key] = cached
        print(f"Prepared {len(segments)} segment(s) for {producer_name}", flush=True)
        return cached

    async def handle_interest(name, param: InterestParam, _app_param):
        relay_base_name = list(remove_chunk_suffix(name))
        seg_no = segment_number(name)

        try:
            obj = await prepare_object(relay_base_name)
            if seg_no >= len(obj.segments):
                print(f"Ignoring out-of-range segment {seg_no} for {Name.to_str(name)}", flush=True)
                return

            data_name = obj.relay_base_name + [Component.from_segment(seg_no)]
            app.put_data(
                data_name,
                content=obj.segments[seg_no],
                freshness_period=FRESHNESS_PERIOD,
                final_block_id=obj.final_segment,
                digest_sha256=True,
            )
            print(f"Served {Name.to_str(data_name)}", flush=True)
        except (InterestNack, InterestTimeout, ValidationFailure) as exc:
            print(f"Producer fetch failed for {Name.to_str(name)}: {exc}", flush=True)
        except Exception as exc:
            print(f"Relay error for {Name.to_str(name)}: {exc}", flush=True)

    @app.route(RELAY_PREFIX)
    def on_interest(name, param: InterestParam, _app_param):
        asyncio.create_task(handle_interest(name, param, _app_param))

    print(f"Relay registering {RELAY_PREFIX}", flush=True)
    app.run_forever()


if __name__ == "__main__":
    if os.environ.get("RELAY_SELF_TEST") == "1":
        _run_self_test()
    else:
        run_relay()
