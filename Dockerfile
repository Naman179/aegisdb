# ── Stage 1: Build ───────────────────────────────────────────────────────────
FROM gcc:13 AS builder

RUN apt-get update && apt-get install -y --no-install-recommends \
    cmake \
    ninja-build \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app
COPY . .

RUN cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
    && cmake --build build -j$(nproc)

# ── Stage 2: Runtime ─────────────────────────────────────────────────────────
FROM debian:bookworm-slim

RUN groupadd -r aegis && useradd -r -g aegis aegis \
    && mkdir -p /data && chown -R aegis:aegis /data

WORKDIR /app

COPY --from=builder /app/build/src/aegisd /usr/local/bin/aegisd
COPY --from=builder /app/build/tools/aegisctl /usr/local/bin/aegisctl
COPY --from=builder /app/build/tools/aegis_bench /usr/local/bin/aegis_bench
COPY --from=builder /app/build/tools/rebalance /usr/local/bin/rebalance

USER aegis
VOLUME ["/data"]

EXPOSE 7001-7005 8001-8005

ENTRYPOINT ["aegisd"]
