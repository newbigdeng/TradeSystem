#!/usr/bin/env bash
set -euo pipefail
source "$(dirname -- "${BASH_SOURCE[0]}")/common.sh"

if [[ "${1:-}" == --help ]]; then
  echo '用法：bash scripts/build.sh [release|debug|all] [目标名 ...]'
  echo '默认 Release 清理后构建；增量构建使用 no_clean_build.sh。'
  exit 0
fi
mode="${1:-release}"
[[ $# -eq 0 ]] || shift
case "${mode,,}" in
  release) modes=(release) ;;
  debug) modes=(debug) ;;
  all) modes=(release debug) ;;
  *) echo "未知构建模式：$mode" >&2; exit 2 ;;
esac
if [[ ${#modes[@]} -gt 1 && -n "${TRADE_BUILD_DIR:-}" ]]; then
  echo 'all 模式不能共用一个 TRADE_BUILD_DIR；请分别构建。' >&2
  exit 2
fi
jobs="${TRADE_BUILD_JOBS:-2}"
clean="${TRADE_CLEAN_BUILD:-1}"
[[ "$jobs" =~ ^[1-9][0-9]*$ && "$clean" =~ ^[01]$ ]] || {
  echo 'TRADE_BUILD_JOBS 必须为正整数，TRADE_CLEAN_BUILD 必须为 0 或 1。' >&2
  exit 2
}
command -v cmake >/dev/null
command -v ninja >/dev/null
for selected in "${modes[@]}"; do
  if [[ "$selected" == release ]]; then
    type=Release; trace="${TRADE_PERF_TRACE_EVERY:-0}"
  else
    type=Debug; trace="${TRADE_PERF_TRACE_EVERY:-1}"
  fi
  directory="$(build_directory_for "$selected")"
  cmake -S "$project_root" -B "$directory" -G Ninja -DCMAKE_BUILD_TYPE="$type" \
    -DTRADE_PERF_TRACE_EVERY="$trace" -DTRADE_SANITIZER="${TRADE_SANITIZER:-}" \
    -DBUILD_TESTING=ON
  command=(cmake --build "$directory" --parallel "$jobs")
  [[ "$clean" == 0 ]] || command+=(--clean-first)
  [[ $# -eq 0 ]] || command+=(--target "$@")
  "${command[@]}"
done
