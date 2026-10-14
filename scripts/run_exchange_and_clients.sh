#!/usr/bin/env bash
set -euo pipefail
source "$(dirname -- "${BASH_SOURCE[0]}")/common.sh"
if [[ "${1:-}" == --help ]]; then
  echo '用法：bash scripts/run_exchange_and_clients.sh [--config 客户端文件] [客户ID 策略 ...]'
  echo '交易所配置使用 TRADE_EXCHANGE_CONFIG；默认运行客户端 10 秒。'
  exit 0
fi

bash "$script_directory/no_clean_build.sh" release exchange_main trading_main
prepare_client_command "$@"
exchange_binary="$(build_directory_for release)/exchange_main"
exchange_config="$(realpath -- "${TRADE_EXCHANGE_CONFIG:-$project_root/config/exchange.json}")"
"$exchange_binary" --config "$exchange_config" --check-config
"$client_binary" --config "$client_config" --check-config "${client_command[@]:3}"
command -v ss >/dev/null
port="$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["tcp"]["port"])' "$exchange_config")"
if [[ -n "${TRADE_RUN_DIR:-}" ]]; then
  run_directory="$(realpath -m -- "$TRADE_RUN_DIR")"
  # 父目录也不复用，避免覆盖前一次的输出或审计。
  mkdir -- "$run_directory"
else
  run_directory="$(mktemp -d "${TMPDIR:-/tmp}/trade-system.XXXXXX")"
fi
mkdir -- "$run_directory/exchange" "$run_directory/client"
echo "联合运行目录：$run_directory"

exchange_pid=''
client_pid=''
cleanup() {
  local status=$?
  trap - EXIT INT TERM
  # 先排空客户端，再停止交易所；只操作本次启动且尚未 wait 的 PID。
  if [[ -n "$client_pid" ]]; then
    kill -TERM "$client_pid" 2>/dev/null || true
    wait "$client_pid" 2>/dev/null || true
  fi
  if [[ -n "$exchange_pid" ]]; then
    kill -TERM "$exchange_pid" 2>/dev/null || true
    wait "$exchange_pid" 2>/dev/null || true
  fi
  return "$status"
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
(cd -- "$run_directory/exchange" && \
  exec env TRADE_SNAPSHOT_MS="${TRADE_SNAPSHOT_MS:-200}" \
  "$exchange_binary" --config "$exchange_config") \
  >"$run_directory/exchange/stdout.log" 2>&1 &
exchange_pid=$!

# ss 必须显示本次交易所 PID 的监听口，旧进程占用端口不能算启动成功。
ready=0
for ((attempt=0; attempt<100; ++attempt)); do
  if ! kill -0 "$exchange_pid" 2>/dev/null; then
    echo "交易所启动失败，查看 $run_directory/exchange/stdout.log" >&2
    exit 1
  fi
  if ss -H -ltnp "sport = :$port" | grep -F "pid=$exchange_pid," >/dev/null; then
    ready=1
    break
  fi
  sleep 0.1
done
[[ "$ready" == 1 ]] || { echo '等待交易所监听超时。' >&2; exit 1; }

(cd -- "$run_directory/client" && \
  exec env TRADE_RUN_SECONDS="${TRADE_RUN_SECONDS:-10}" "${client_command[@]}") \
  >"$run_directory/client/stdout.log" 2>&1 &
client_pid=$!
status=0
wait "$client_pid" || status=$?
client_pid=''
if [[ "$status" != 0 ]]; then
  echo "客户端退出码 $status，查看 $run_directory/client/stdout.log" >&2
fi
kill -TERM "$exchange_pid" 2>/dev/null || true
exchange_status=0
wait "$exchange_pid" || exchange_status=$?
exchange_pid=''
[[ "$status" != 0 ]] || status="$exchange_status"
exit "$status"
