#!/usr/bin/env bash
# ─────────────────────────────────────────────────────────────────────────────
# stop_cluster.sh – Gracefully stop all local AegisDB cluster nodes
# ─────────────────────────────────────────────────────────────────────────────
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"

for id in {1..5}; do
    PID_FILE="${ROOT_DIR}/run/node${id}.pid"
    if [[ -f "${PID_FILE}" ]]; then
        PID=$(cat "${PID_FILE}")
        if kill -0 "${PID}" 2>/dev/null; then
            echo "Stopping node ${id} (PID ${PID})..."
            kill "${PID}" 2>/dev/null || true
        fi
        rm -f "${PID_FILE}"
    fi
done

echo "All cluster nodes stopped."
