#!/usr/bin/env bash
# 文件：run.sh
# 作用：本机参赛启动入口
# 加载 ROS2 与通信环境，执行 install/lib/rmtc_bot/rmtc_node，并透传命令行参数。
# 原 ROS 日志目录不可写时回退 /tmp，避免日志挂载故障阻止启动；终端打印实际目录。
# 用法：./run.sh --stage 1 --seat A；先 ./build.sh，阶段2另配网关与令牌。

set -e
ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
cd "$ROOT"
source /opt/ros/humble/setup.bash
export ROS_DOMAIN_ID="${ROS_DOMAIN_ID:-27}"
export ROS_LOCALHOST_ONLY="${ROS_LOCALHOST_ONLY:-0}"
export RMW_IMPLEMENTATION="${RMW_IMPLEMENTATION:-rmw_fastrtps_cpp}"
export ROS_LOG_DIR="${ROS_LOG_DIR:-$ROOT/logs/ros}"
if mkdir -p "$ROS_LOG_DIR" 2>/dev/null && ROS_LOG_PROBE=$(mktemp "$ROS_LOG_DIR/.rmtc-check.XXXXXX" 2>/dev/null); then
  rm -f "$ROS_LOG_PROBE"
else
  export ROS_LOG_DIR
  ROS_LOG_DIR=$(mktemp -d /tmp/rmtc-ros.XXXXXX) || { printf '无法创建 ROS 日志备用目录\n' >&2; exit 2; }
  printf 'ROS 日志目录不可写，已回退：%s\n' "$ROS_LOG_DIR" >&2
fi
exec "$ROOT/install/lib/rmtc_bot/rmtc_node" "$@"
