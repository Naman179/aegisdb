#!/usr/bin/env bash
# ─────────────────────────────────────────────────────────────────────────────
# kill_node.sh – Terminate a specific AegisDB node by ID (supports -9 simulation)
# ─────────────────────────────────────────────────────────────────────────────
set -euo pipefail

if [[ $# -lt 1 ]]; then
    echo "Usage: $0 <node_id> [signal]"
    echo "Example: $0 1 -9"
    exit 1
fi

NODE_ID="$1"
SIG="${2:--9}"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
PID_FILE="${ROOT_DIR}/run/node${NODE_ID}.pid"

if [[ -f "${PID_FILE}" ]]; then
    PID=$(cat "${PID_FILE}")
    if kill -0 "${PID}" 2>/dev/null; then
        echo "Killing node ${NODE_ID} (PID ${PID}) with signal ${SIG}..."
        kill "${SIG}" "${PID}" 2>/dev/null || true
    fi
    rm -f "${PID_FILE}"
else
    echo "Node ${NODE_ID} PID file not found."
fi
