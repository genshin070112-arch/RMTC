#!/usr/bin/env bash
# 文件：tools/seal.sh
# 作用：生成源码校验清单
# 对指定源码、第三方库和构建/启动文件计算 SHA256，写入 source_manifest.sha256。
# 用法：./tools/seal.sh；会更新清单。策略配置与令牌不在源码封存范围内。

set -e
ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"
{ find src include third_party tests launch tools -type f ! -path '*/__pycache__/*' ! -name '*.pyc' -print0; printf '%s\0' CMakeLists.txt package.xml build.sh run.sh activate.sh run_container.sh Dockerfile .dockerignore; } \
  | sort -z | xargs -0 sha256sum > source_manifest.sha256
printf '已记录源码版本；decision_config.json 与令牌不在源码校验表内。\n'
