#!/usr/bin/env bash
# 文件：run_container.sh
# 作用：容器参赛启动入口
# 用主机网络/IPC 启动 rmtc-cpp:1.1，挂载配置、令牌和日志，并透传参数。
# 用法：./run_container.sh --stage 1；需要先构建或载入镜像，域与席位按裁判通知设置。

set -e
ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
mkdir -p "$ROOT/logs/container" "$ROOT/.secrets"
# set_token.sh 的默认文件挂载后必须显式传给 C++ 配置读取器。
# 显式文件/环境令牌优先；--token-file 命令行参数仍可覆盖此默认值。
TOKEN_FILE="${RMTC_TOKEN_FILE:-}"
if [[ -z "$TOKEN_FILE" && -z "${RMTC_TEAM_TOKEN:-}" && -f "$ROOT/.secrets/team.token" ]]; then
  TOKEN_FILE=/app/.secrets/team.token
fi
exec docker run --rm --init --network host --ipc host \
  --user "$(id -u):$(id -g)" \
  -e "ROS_DOMAIN_ID=${ROS_DOMAIN_ID:-27}" \
  -e "RMW_IMPLEMENTATION=${RMW_IMPLEMENTATION:-rmw_fastrtps_cpp}" \
  -e "RMTC_TOKEN_FILE=$TOKEN_FILE" \
  -e "RMTC_TEAM_TOKEN=${RMTC_TEAM_TOKEN:-}" \
  -v "$ROOT/config:/app/config:ro" \
  -v "$ROOT/.secrets:/app/.secrets:ro" \
  -v "$ROOT/logs/container:/app/logs" \
  rmtc-cpp:1.1 "$@"
