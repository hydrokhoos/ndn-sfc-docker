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

set_strategy() {
  strategy_name="${STRATEGY_NAME:-}"
  if [ -z "${strategy_name}" ]; then
    return 0
  fi

  case "${strategy_name}" in
    best-route)
      strategy_uri="/localhost/nfd/strategy/best-route"
      ;;
    least-pending-interests|lpi)
      strategy_uri="/localhost/nfd/strategy/least-pending-interests"
      ;;
    remaining-chain-cost|rcc)
      strategy_uri="/localhost/nfd/strategy/remaining-chain-cost"
      ;;
    /localhost/nfd/strategy/*)
      strategy_uri="${strategy_name}"
      ;;
    *)
      echo "Unknown STRATEGY_NAME=${strategy_name}" >&2
      return 1
      ;;
  esac

  for prefix in ${STRATEGY_PREFIXES:-/sfc /content}; do
    echo "Setting strategy ${prefix} -> ${strategy_uri}"
    retry nfdc strategy set "${prefix}" "${strategy_uri}"
  done
}

add_route() {
  route="$1"
  prefix="${route%%=*}"
  route_rest="${route#*=}"
  cost=""

  if [ "${route_rest}" != "${route_rest#*=}" ]; then
    next_hop="${route_rest%%=*}"
    cost="${route_rest#*=}"
  elif [ "${route_rest}" != "${route_rest%:*}" ]; then
    next_hop="${route_rest%%:*}"
    cost="${route_rest#*:}"
  else
    next_hop="${route_rest}"
  fi

  if [ -n "${cost}" ]; then
    echo "Adding route ${prefix} via udp://${next_hop} cost=${cost}"
    retry nfdc route add "${prefix}" "udp://${next_hop}" cost "${cost}"
  else
    echo "Adding route ${prefix} via udp://${next_hop}"
    retry nfdc route add "${prefix}" "udp://${next_hop}"
  fi
}

mkdir -p /run/nfd
nfd-start 2> /var/log/nfd.log
wait_for_nfd
set_strategy

for neighbor in ${NFD_NEIGHBORS:-}; do
  echo "Creating face udp://${neighbor}"
  retry nfdc face create "udp://${neighbor}"
done

for route in ${NFD_ROUTES:-}; do
  add_route "${route}"
done

echo "NFD is running on tcp4://0.0.0.0:6363"
tail -f /dev/null
