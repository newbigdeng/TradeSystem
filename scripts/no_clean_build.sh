#!/usr/bin/env bash
set -euo pipefail
# 与清理构建共用参数和目录，避免两个入口逐渐不一致。
exec env TRADE_CLEAN_BUILD=0 bash "$(dirname -- "${BASH_SOURCE[0]}")/build.sh" "$@"
