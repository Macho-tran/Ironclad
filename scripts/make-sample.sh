#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
#
# scripts/make-sample.sh — produce a small, deterministic sample
# .iclr recording for the Replay Studio demo.
#
# Defaults to the same params CI uses for the soak smoke test:
# 30s @ 60Hz, 4 AI players, 150ms RTT, 30ms jitter, 5% loss,
# 2% reorder. The output is byte-identical for a given seed and
# arena_demo binary.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
BUILD="${BUILD_DIR:-$ROOT/build}"
OUT="${OUT_PATH:-$ROOT/samples/sample.iclr}"

if [[ ! -x "$BUILD/apps/arena_demo/arena_demo" ]]; then
    echo "arena_demo not built. Try:  cmake -S '$ROOT' -B '$BUILD' && cmake --build '$BUILD'" >&2
    exit 1
fi

mkdir -p "$(dirname "$OUT")"

"$BUILD/apps/arena_demo/arena_demo" \
    --frames "${FRAMES:-1800}" \
    --players "${PLAYERS:-4}" \
    --rtt-ms "${RTT_MS:-150}" \
    --jitter-ms "${JITTER_MS:-30}" \
    --loss-pct "${LOSS_PCT:-5}" \
    --reorder-pct "${REORDER_PCT:-2}" \
    --seed "${SEED:-0xC0FFEEBEEFD00D}" \
    --record "$OUT" \
    --quiet

echo "wrote $OUT"
"$BUILD/apps/arena_demo/arena_demo" --replay-info "$OUT" | head -16
