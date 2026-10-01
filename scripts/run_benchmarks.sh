#!/usr/bin/env bash
set -euo pipefail
project_root="$(cd "$(dirname "$0")/.." && pwd)"
cd "$project_root"
build_directory="${TRADE_BENCH_BUILD:-build/release}"
output_directory="${1:-$HOME/trade-measurements/$(date -u +%Y%m%dT%H%M%SZ)}"
cmake -S . -B "$build_directory" -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build "$build_directory" -j 4
python3 record/02_reproducible_measurement/run_measurements.py \
  --build "$build_directory" --output "$output_directory"
