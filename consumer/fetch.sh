#!/usr/bin/env bash
set -euo pipefail

CONTENT_NAMES="${CONTENT_NAMES:-/relay/sample.txt}"
WAIT_PREFIXES="${WAIT_PREFIXES:-/sample.txt /relay}"

wait_for_nfd() {
  echo "Waiting for shared NFD..."
  for _ in $(seq 1 60); do
    if nfdc status >/dev/null 2>&1; then
      echo "Shared NFD is ready."
      return 0
    fi
    sleep 1
  done

  echo "Timed out waiting for shared NFD." >&2
  return 1
}

wait_for_prefix() {
  prefix="$1"
  echo "Waiting for route ${prefix}..."
  for _ in $(seq 1 60); do
    if nfdc fib 2>/dev/null | grep -q "${prefix}"; then
      echo "Route ${prefix} is ready."
      return 0
    fi
    sleep 1
  done

  echo "Timed out waiting for route ${prefix}." >&2
  return 1
}

wait_for_nfd
for prefix in ${WAIT_PREFIXES}; do
  wait_for_prefix "${prefix}"
done

for content_name in ${CONTENT_NAMES}; do
  echo "Fetching ${content_name}"
  ndncatchunks -D -f "${content_name}"
done
