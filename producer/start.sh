#!/usr/bin/env bash
set -euo pipefail

CONTENT_NAME="${CONTENT_NAME:-/sample.txt}"
CONTENT_TEXT="${CONTENT_TEXT:-Hello from NDN producer.}"
CONTENT_FILE="/tmp/sample.txt"

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

wait_for_nfd

printf "%s\n" "${CONTENT_TEXT}" > "${CONTENT_FILE}"

echo "Publishing ${CONTENT_NAME}"
exec ndnputchunks -S id:/localhost/identity/digest-sha256 "${CONTENT_NAME}" < "${CONTENT_FILE}"
