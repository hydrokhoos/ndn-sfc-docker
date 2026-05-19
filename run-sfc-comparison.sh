#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
COMPOSE_FILE="${ROOT_DIR}/docker-compose.comparison.yaml"
PROJECT_NAME="${PROJECT_NAME:-ndn_sfc_cmp}"
REQUEST_COUNT="${REQUEST_COUNT:-10}"
CONCURRENCY="${CONCURRENCY:-1}"
RUNS="${RUNS:-1}"
SERVICE_DELAY_MS="${SERVICE_DELAY_MS:-20}"
SLOW_SERVICE_DELAY_MS="${SLOW_SERVICE_DELAY_MS:-80}"
CONTENT_DELAY_MS="${CONTENT_DELAY_MS:-10}"
SLOW_CONTENT_DELAY_MS="${SLOW_CONTENT_DELAY_MS:-80}"
INTEREST_LIFETIME_MS="${INTEREST_LIFETIME_MS:-6000}"
RESULTS_DIR="${RESULTS_DIR:-${ROOT_DIR}/comparison-results/$(date +%Y%m%d-%H%M%S)}"
CSV_FILE="${RESULTS_DIR}/summary.csv"

STRATEGIES="${STRATEGIES:-best-route least-pending-interests remaining-chain-cost}"
SCENARIOS="${SCENARIOS:-content-only single-chain deep-chain}"

mkdir -p "${RESULTS_DIR}"
printf "strategy,scenario,run_id,request_count,success_count,timeout_count,convergence_ms,response_avg_ms,response_p50_ms,response_p95_ms,response_max_ms\n" > "${CSV_FILE}"

compose() {
  docker compose -p "${PROJECT_NAME}" -f "${COMPOSE_FILE}" "$@"
}

cleanup_stack() {
  compose down --remove-orphans >/dev/null 2>&1 || true
}

required_prefixes() {
  case "$1" in
    content-only)
      printf "/content\n"
      ;;
    single-chain)
      printf "/sfc/resize\n/svc/resize\n/content\n"
      ;;
    deep-chain)
      printf "/sfc/resize\n/svc/resize\n/sfc/compress\n/svc/compress\n/sfc/filter\n/svc/filter\n/content\n"
      ;;
    *)
      echo "unknown scenario: $1" >&2
      return 1
      ;;
  esac
}

wait_for_nfd1() {
  for _ in $(seq 1 120); do
    if compose exec -T nfd1 nfdc status >/dev/null 2>&1; then
      return 0
    fi
    sleep 0.5
  done
  return 1
}

wait_for_prefixes() {
  scenario="$1"
  for _ in $(seq 1 120); do
    fib="$(compose exec -T nfd1 nfdc fib 2>/dev/null || true)"
    ok=1
    while IFS= read -r prefix; do
      if [ -n "${prefix}" ] && ! printf "%s\n" "${fib}" | grep -q "${prefix}"; then
        ok=0
      fi
    done < <(required_prefixes "${scenario}")
    if [ "${ok}" -eq 1 ]; then
      return 0
    fi
    sleep 0.5
  done
  return 1
}

run_consumer_json() {
  scenario="$1"
  count="$2"
  start_seq="$3"
  compose exec -T consumer python3 /app/consumer.py \
    --scenario "${scenario}" \
    --count "${count}" \
    --start-seq "${start_seq}" \
    --concurrency "${CONCURRENCY}" \
    --lifetime-ms "${INTEREST_LIFETIME_MS}" \
    --json
}

extract_json_line() {
  awk '/^\{.*\}$/ { line=$0 } END { if (line != "") print line; else exit 1; }'
}

append_csv_row() {
  strategy="$1"
  scenario="$2"
  run_id="$3"
  convergence_ms="$4"
  json="$5"
  python3 - "$strategy" "$scenario" "$run_id" "$convergence_ms" "$json" >> "${CSV_FILE}" <<'PY'
import json
import sys

strategy, scenario, run_id, convergence_ms, payload = sys.argv[1:]
data = json.loads(payload)
values = [
    strategy,
    scenario,
    run_id,
    str(data["request_count"]),
    str(data["success_count"]),
    str(data["timeout_count"]),
    str(int(float(convergence_ms))),
    f'{float(data["response_avg_ms"]):.3f}',
    f'{float(data["response_p50_ms"]):.3f}',
    f'{float(data["response_p95_ms"]):.3f}',
    f'{float(data["response_max_ms"]):.3f}',
]
print(",".join(values))
PY
}

save_logs() {
  run_dir="$1"
  mkdir -p "${run_dir}"
  compose logs --no-color > "${run_dir}/compose.log" 2>&1 || true
  for nfd in nfd1 nfd2 nfd3; do
    cid="$(compose ps -q "${nfd}" 2>/dev/null || true)"
    if [ -n "${cid}" ]; then
      docker cp "${cid}:/var/log/nfd.log" "${run_dir}/${nfd}.log" >/dev/null 2>&1 || true
    fi
  done
}

print_summary() {
  python3 - "${CSV_FILE}" <<'PY'
import csv
import sys

with open(sys.argv[1], newline="") as f:
    rows = list(csv.DictReader(f))

print()
print("SFC strategy comparison summary")
print("strategy                 scenario       success/total  conv_ms  avg_ms   p50_ms   p95_ms   max_ms")
for row in rows:
    print(f'{row["strategy"]:<24} {row["scenario"]:<14} '
          f'{row["success_count"]}/{row["request_count"]:<10} '
          f'{row["convergence_ms"]:>7} '
          f'{float(row["response_avg_ms"]):>7.1f} '
          f'{float(row["response_p50_ms"]):>7.1f} '
          f'{float(row["response_p95_ms"]):>7.1f} '
          f'{float(row["response_max_ms"]):>7.1f}')
print()
print(f"CSV: {sys.argv[1]}")
PY
}

trap cleanup_stack EXIT

echo "Writing results to ${RESULTS_DIR}"
echo "Building custom NFD image..."
compose build nfd1

for run_id in $(seq 1 "${RUNS}"); do
  for strategy in ${STRATEGIES}; do
    for scenario in ${SCENARIOS}; do
      run_label="${strategy}-${scenario}-run${run_id}"
      run_dir="${RESULTS_DIR}/${run_label}"
      echo
      echo "=== ${run_label} ==="

      cleanup_stack
      start_ms="$(python3 - <<'PY'
import time
print(int(time.time() * 1000))
PY
)"

      STRATEGY_NAME="${strategy}" \
      SERVICE_DELAY_MS="${SERVICE_DELAY_MS}" \
      SLOW_SERVICE_DELAY_MS="${SLOW_SERVICE_DELAY_MS}" \
      CONTENT_DELAY_MS="${CONTENT_DELAY_MS}" \
      SLOW_CONTENT_DELAY_MS="${SLOW_CONTENT_DELAY_MS}" \
      compose up -d

      wait_for_nfd1
      wait_for_prefixes "${scenario}"

      canary_output=""
      canary_json=""
      for _ in $(seq 1 80); do
        set +e
        canary_output="$(run_consumer_json "${scenario}" 1 "$((run_id * 100000))" 2>&1)"
        canary_rc=$?
        set -e
        if [ "${canary_rc}" -eq 0 ] && canary_json="$(printf "%s\n" "${canary_output}" | extract_json_line)" &&
           python3 - "${canary_json}" <<'PY'
import json
import sys
sys.exit(0 if json.loads(sys.argv[1]).get("success_count") == 1 else 1)
PY
        then
          break
        fi
        canary_json=""
        sleep 0.5
      done

      if [ -z "${canary_json}" ]; then
        echo "ERROR: canary fetch did not succeed for ${run_label}" >&2
        printf "%s\n" "${canary_output}" >&2
        save_logs "${run_dir}"
        exit 1
      fi

      end_ms="$(python3 - <<'PY'
import time
print(int(time.time() * 1000))
PY
)"
      convergence_ms=$((end_ms - start_ms))

      measurement_output="$(run_consumer_json "${scenario}" "${REQUEST_COUNT}" "$((run_id * 100000 + 1))" 2>&1)"
      measurement_json="$(printf "%s\n" "${measurement_output}" | extract_json_line)"
      append_csv_row "${strategy}" "${scenario}" "${run_id}" "${convergence_ms}" "${measurement_json}"
      save_logs "${run_dir}"
      echo "convergence_ms=${convergence_ms}"
      echo "${measurement_json}"
    done
  done
done

cleanup_stack
print_summary
