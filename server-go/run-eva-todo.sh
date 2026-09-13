#!/bin/bash
# launchd 启动脚本: 读 server-go/.env 后 exec 本机 Todo 服务器
# 由 Hermes 生成。token 只存在 .env 里, 不写进 plist。
cd "$(dirname "$0")" || exit 1
set -a
. ./.env
set +a
export LISTEN_ADDR="${LISTEN_ADDR:-:8080}"
export DATA_FILE="$PWD/data/eva-todo.json"
exec ./eva-todo-server
