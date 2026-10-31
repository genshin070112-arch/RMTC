#!/usr/bin/env bash
# 文件：activate.sh
# 作用：ROS2 包发现环境
# 加载 Humble 并把本工程的 install/ 加入包搜索路径，供直接 CMake 安装后使用 ros2 CLI。
# 用法：source ./activate.sh；必须 source，单独执行不会修改当前终端环境。

# Source this file after a direct CMake build to expose the package to ros2 CLI.
RMTC_ACTIVATE_ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
source /opt/ros/humble/setup.bash
export AMENT_PREFIX_PATH="$RMTC_ACTIVATE_ROOT/install${AMENT_PREFIX_PATH:+:$AMENT_PREFIX_PATH}"
unset RMTC_ACTIVATE_ROOT
