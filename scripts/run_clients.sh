#!/usr/bin/env bash
set -euo pipefail
source "$(dirname -- "${BASH_SOURCE[0]}")/common.sh"
if [[ "${1:-}" == --help ]]; then
  echo '用法：bash scripts/run_clients.sh [--config 文件] [客户ID 策略 [品种参数 ...]]'
  echo '不传位置参数时，客户 ID、策略及风控全部使用客户端 JSON。'
  exit 0
fi
prepare_client_command "$@"
if [[ -n "${TRADE_RUN_DIR:-}" ]]; then
  run_directory="$(realpath -m -- "$TRADE_RUN_DIR")"
  mkdir -- "$run_directory"
else
  run_directory="$(mktemp -d "${TMPDIR:-/tmp}/trade-client.XXXXXX")"
fi
echo "客户端运行目录：$run_directory"
cd -- "$run_directory"
# exec 使外部脚本保存的 PID 就是客户端，信号直接进入程序的排空流程。
exec "${client_command[@]}"
