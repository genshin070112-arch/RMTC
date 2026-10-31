#!/usr/bin/env bash
# 文件：tools/check_seal.sh
# 作用：复核源码与策略配置
# 检查 source_manifest.sha256；若已安装 jq，再检查策略 JSON 仅含允许的 decision_type。
# 用法：./tools/check_seal.sh；返回非零即检查失败，不能用它证明比赛策略最优。

set -e
ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"
sha256sum -c source_manifest.sha256
if command -v jq >/dev/null; then
  jq -e 'type=="object" and keys==["decision_type"] and (.decision_type=="balanced" or .decision_type=="collect" or .decision_type=="combat" or .decision_type=="safe")' config/decision_config.json >/dev/null
fi
