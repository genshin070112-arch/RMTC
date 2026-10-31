#!/usr/bin/env bash
# 文件：tools/verify_resilience.sh
# 作用：在隔离 ROS 域运行实际 C++ 节点和 C++ 故障探针，保存完整诊断。
# 仅用于人工离线验收；不访问裁判管理数据，不调用真实模型，不安装进参赛镜像。
# 用法：ROS_DOMAIN_ID=103 bash tools/verify_resilience.sh [证据目录] [normal|unwritable]
# unwritable 用 /proc 下无法创建的日志目录验证日志故障降级；结束时只停止本脚本启动的进程。
set -euo pipefail
ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
OUT="${1:-$ROOT/evidence/review2_ros_faults}"
MODE="${2:-normal}"
mkdir -p "$OUT"
OUT="$(cd "$OUT" && pwd)"
if [[ "$MODE" != normal && "$MODE" != unwritable ]]; then
  printf 'mode must be normal or unwritable\n' >&2
  exit 2
fi
export ROS_DOMAIN_ID="${ROS_DOMAIN_ID:-103}"
export ROS_LOCALHOST_ONLY=0
export RMW_IMPLEMENTATION=rmw_fastrtps_cpp
export RMTC_TEAM_TOKEN=''
set +u
source /opt/ros/humble/setup.bash
set -u
LOG_DIR="$OUT/bot"
if [[ "$MODE" == unwritable ]]; then LOG_DIR=/proc/rmtc-unwritable-audit; fi
export ROS_LOG_DIR="$OUT/ros"
if [[ "$MODE" == unwritable ]]; then export ROS_LOG_DIR=/proc/rmtc-unwritable-ros; fi
"$ROOT/run.sh" --stage 2 --seat A \
  --decision-file "$ROOT/config/decision_config.json" --log-dir "$LOG_DIR" \
  --gateway http://127.0.0.1:1 > "$OUT/node.log" 2>&1 &
BOT_PID=$!
cleanup() {
  if kill -0 "$BOT_PID" 2>/dev/null; then kill -INT "$BOT_PID"; fi
  wait "$BOT_PID" 2>/dev/null || true
}
trap cleanup EXIT
sleep 3.6
ROS_LOG_DIR="$OUT/probe_ros" timeout 35 "$ROOT/build/ros_fault_probe" > "$OUT/probe.log" 2>&1
rg -q no_initial_perception_check_domain_seat_network "$OUT/node.log"
rg -q perception_resumed "$OUT/node.log"
rg -q no_recent_perception_no_stale_actions "$OUT/node.log"
cat "$OUT/probe.log"
# 错误回执证明通信已通，不能误报“没有回执”；动作是否成功另看回执内容。
if rg -q '"status":"missing_action_receipts_check_bidirectional_DDS","tick":10[5-6]' "$OUT/node.log"; then
  printf 'FAIL error_receipts_misclassified_as_missing_DDS\n' >&2
  exit 1
fi
printf 'PASS error_receipts_are_communication_acknowledgements\n'
