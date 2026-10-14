#!/usr/bin/env bash
# 供同目录脚本复用路径与配置解析；不启动进程。
script_directory="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
project_root="$(cd -- "$script_directory/.." && pwd)"

build_directory_for() {
  local directory="${TRADE_BUILD_DIR:-build/$1}"
  [[ "$directory" = /* ]] || directory="$project_root/$directory"
  realpath -m -- "$directory"
}

prepare_client_command() {
  local config="${TRADE_CLIENT_CONFIG:-$project_root/config/client.json}"
  if [[ "${1:-}" == --config ]]; then
    if [[ $# -lt 2 || -z "$2" ]]; then
      echo '--config 需要一个 JSON 文件路径' >&2
      return 2
    fi
    config="$2"
    shift 2
  fi
  client_config="$(realpath -- "$config")"
  client_binary="$(build_directory_for release)/trading_main"
  [[ -x "$client_binary" ]] || {
    echo "缺少客户端可执行文件：$client_binary；请先运行 scripts/no_clean_build.sh" >&2
    return 2
  }
  client_command=("$client_binary" --config "$client_config" "$@")
}
