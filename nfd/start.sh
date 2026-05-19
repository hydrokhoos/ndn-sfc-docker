#!/usr/bin/env bash
set -euo pipefail

wait_for_nfd() {
  echo "Waiting for NFD daemon..."
  for _ in $(seq 1 60); do
    if nfdc status >/dev/null 2>&1; then
      echo "NFD is ready."
      return 0
    fi
    sleep 1
  done

  echo "Timed out waiting for NFD daemon." >&2
  return 1
}

retry() {
  for _ in $(seq 1 30); do
    if "$@"; then
      return 0
    fi
    sleep 1
  done

  echo "Command failed after retries: $*" >&2
  return 1
}

mkdir -p /run/nfd
nfd-start 2> /var/log/nfd.log
wait_for_nfd

for neighbor in ${NFD_NEIGHBORS:-}; do
  echo "Creating face udp://${neighbor}"
  retry nfdc face create "udp://${neighbor}"
done

for route in ${NFD_ROUTES:-}; do
  prefix="${route%%=*}"
  next_hop="${route#*=}"
  echo "Adding route ${prefix} via udp://${next_hop}"
  retry nfdc route add "${prefix}" "udp://${next_hop}"
done

echo "NFD is running on tcp4://0.0.0.0:6363"
tail -f /dev/null
