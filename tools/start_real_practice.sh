#!/usr/bin/env bash
# 文件：tools/start_real_practice.sh
# 作用：启动真实模型练习端
# 在终端读取上游 URL、模型名、密钥，交给原始练习端启动其 real 模式代理。
# 用法：./tools/start_real_practice.sh；先准备模型服务。PRACTICE_DIR 可指定原始练习端目录。
# 它不是参赛客户端，也不自己实现网关服务；参赛 C++ 仍访问练习端提供的地址。

set -e
ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
PRACTICE_DIR="${PRACTICE_DIR:-/home/yuanzh/桌面/校内赛/新规则/dist/rmtc_practice}"
read -r -p '自备模型的完整 /v1/chat/completions 地址: ' PRACTICE_LLM_URL
read -r -p '模型名称: ' PRACTICE_MODEL
read -r -s -p '模型 API Key（不会回显，不会写文件）: ' PRACTICE_LLM_KEY
printf '\n'
export PRACTICE_LLM_URL PRACTICE_MODEL PRACTICE_LLM_KEY
export ROS_DOMAIN_ID="${ROS_DOMAIN_ID:-73}"
export RMTC_INTERNAL_DOMAIN_ID="${RMTC_INTERNAL_DOMAIN_ID:-74}"
export ROS_LOG_DIR="${ROS_LOG_DIR:-$ROOT/logs/referee_ros}"
exec "$PRACTICE_DIR/start_referee.sh" --stage 2 --port "${PRACTICE_PORT:-18081}" --ai-mode real --data-dir "$ROOT/logs/real_practice"
