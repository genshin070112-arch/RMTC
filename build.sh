#!/usr/bin/env bash
# 文件：build.sh
# 作用：本机编译安装入口
# 加载 ROS2 Humble，用 CMake 编译 C++ 与测试，再安装到本工程 install/。
# 用法：./build.sh；生成 build/、install/，它们是产物目录，不是要手改的源代码。

set -e
ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
source /opt/ros/humble/setup.bash
cmake -S "$ROOT" -B "$ROOT/build" -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$ROOT/install" -DBUILD_TESTING=ON
cmake --build "$ROOT/build" -j2
cmake --install "$ROOT/build"
