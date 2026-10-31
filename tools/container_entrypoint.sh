#!/usr/bin/env bash
# 文件：tools/container_entrypoint.sh
# 作用：容器内部启动脚本
# 加载 Humble、准备日志目录，再 exec 镜像里的 C++ rmtc_node。
# 挂载日志不可写时回退容器 /tmp，并打印实际路径；容器退出后需另存临时日志。
# 由 Docker ENTRYPOINT 调用；它只启动程序，不承担决策。

set -e
source /opt/ros/humble/setup.bash
export ROS_LOG_DIR="${ROS_LOG_DIR:-/app/logs/ros}"
if mkdir -p "$ROS_LOG_DIR" 2>/dev/null && ROS_LOG_PROBE=$(mktemp "$ROS_LOG_DIR/.rmtc-check.XXXXXX" 2>/dev/null); then
  rm -f "$ROS_LOG_PROBE"
else
  export ROS_LOG_DIR
  ROS_LOG_DIR=$(mktemp -d /tmp/rmtc-ros.XXXXXX) || { printf '无法创建 ROS 日志备用目录\n' >&2; exit 2; }
  printf 'ROS 日志目录不可写，已回退：%s\n' "$ROS_LOG_DIR" >&2
fi
exec /opt/rmtc/lib/rmtc_bot/rmtc_node "$@"
