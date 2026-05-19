#!/usr/bin/env bash
set -euo pipefail

echo "Waiting for shared NFD..."
for _ in $(seq 1 60); do
  if nfdc status >/dev/null 2>&1; then
    echo "Shared NFD is ready."
    break
  fi
  sleep 1
done

if ! nfdc status >/dev/null 2>&1; then
  echo "Timed out waiting for shared NFD." >&2
  exit 1
fi

ndnsec key-gen /relay | ndnsec cert-install - || true
exec python3 relay.py
