#!/usr/bin/env bash
set -euo pipefail
source "$(dirname -- "${BASH_SOURCE[0]}")/common.sh"
if [[ "${1:-}" == --help ]]; then
  echo '用法：bash scripts/run_benchmarks.sh [新输出目录]'
  echo '只测现有组件；默认 7 轮。不是网络往返或旧/新源码对照。'
  exit 0
fi
[[ $# -le 1 ]] || { echo '最多传入一个输出目录。' >&2; exit 2; }
rounds="${TRADE_BENCH_ROUNDS:-7}"
[[ "$rounds" =~ ^[1-9][0-9]*$ ]] || { echo 'TRADE_BENCH_ROUNDS 必须为正整数。' >&2; exit 2; }
export TRADE_BUILD_DIR="${TRADE_BENCH_BUILD:-${TRADE_BUILD_DIR:-build/release}}"
# 基准入口明确关闭诊断计时，避免继承旧 CMake 缓存中的采样设置。
export TRADE_PERF_TRACE_EVERY=0
export TRADE_SANITIZER=''
programs=(release_benchmark logger_benchmark hash_benchmark measurement_component_benchmark)
if [[ $# -eq 1 ]]; then
  output_directory="$(realpath -m -- "$1")"
  mkdir -- "$output_directory"
else
  mkdir -p -- "$HOME/trade-measurements"
  output_directory="$(mktemp -d "$HOME/trade-measurements/components.XXXXXX")"
fi
echo "组件基准输出目录：$output_directory"
bash "$script_directory/no_clean_build.sh" release "${programs[@]}" \
  >"$output_directory/build.log" 2>&1
build_directory="$(build_directory_for release)"
{
  echo 'scope=current component benchmarks; no network RTT; no old/new comparison'
  echo "rounds=$rounds"
  echo "build=$build_directory"
  echo 'trace=0; sanitizer=none'
  git -C "$project_root" rev-parse HEAD
  git -C "$project_root" status --short
  uname -a
  cmake --version
  grep -E 'CMAKE_BUILD_TYPE:|TRADE_PERF_TRACE_EVERY:|TRADE_SANITIZER:' "$build_directory/CMakeCache.txt"
} >"$output_directory/environment.txt"
for ((round=1; round<=rounds; ++round)); do
  for program in "${programs[@]}"; do
    directory="$output_directory/round-$round/$program"
    mkdir -p -- "$directory"
    args=()
    # 前三项支持交换内部两组对照的顺序，组件集合只有模式参数。
    if (( round % 2 == 0 )) && [[ "$program" != measurement_component_benchmark ]]; then
      args=(reverse)
    fi
    (cd -- "$directory" && "$build_directory/$program" "${args[@]}") \
      >"$directory/result.txt" 2>"$directory/stderr.txt"
  done
done
echo "已完成 $rounds 轮组件测量，原始输出保存在 $output_directory"
