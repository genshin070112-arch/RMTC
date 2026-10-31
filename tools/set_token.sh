#!/usr/bin/env bash
# 文件：tools/set_token.sh
# 作用：安全填写比赛令牌
# 终端隐藏输入令牌，以限制权限写入本项目 .secrets/team.token。
# 用法：./tools/set_token.sh；此文件是令牌输入工具，真实令牌不应写进源码或聊天。

set -e
ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
umask 077
mkdir -p "$ROOT/.secrets"
read -r -s -p '输入本场本席位令牌（不会回显）: ' match_token
printf '\n'
if [[ -z "$match_token" ]]; then echo '令牌不能为空' >&2; exit 2; fi
printf '%s\n' "$match_token" > "$ROOT/.secrets/team.token"
unset match_token
printf '已写入本地 .secrets/team.token；不会进入源码包或镜像。\n'
