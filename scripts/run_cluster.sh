#!/usr/bin/env bash
# ─────────────────────────────────────────────────────────────────────────────
# run_cluster.sh – Start a 5-node AegisDB cluster on localhost
# ─────────────────────────────────────────────────────────────────────────────
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
BIN="${ROOT_DIR}/build/aegisd"

if [[ ! -x "${BIN}" ]]; then
    echo "Error: ${BIN} not found. Please build the project first."
    exit 1
fi

mkdir -p "${ROOT_DIR}/run/logs" "${ROOT_DIR}/run/data"

PEER_LIST="1:127.0.0.1:7001:8001,2:127.0.0.1:7002:8002,3:127.0.0.1:7003:8003,4:127.0.0.1:7004:8004,5:127.0.0.1:7005:8005"

for id in {1..5}; do
    DATA_DIR="${ROOT_DIR}/run/data/node${id}"
    LOG_FILE="${ROOT_DIR}/run/logs/node${id}.log"
    mkdir -p "${DATA_DIR}"

    echo "Starting node ${id}..."
    "${BIN}" --id "${id}" \
             --client-port "$((7000 + id))" \
             --peer-port "$((8000 + id))" \
             --data-dir "${DATA_DIR}" \
             --peers "${PEER_LIST}" > "${LOG_FILE}" 2>&1 &
    echo $! > "${ROOT_DIR}/run/node${id}.pid"
done

echo "Cluster started (5 nodes, 3 Raft groups)."
echo "Client ports: 7001..7005 | Peer ports: 8001..8005"
echo "Check status with: ./build/aegisctl --node 127.0.0.1:7001 status"
