#!/bin/bash
# 一键启动 USB Todo 网页端(自动发现串口 + 自动打开浏览器)
cd "$(dirname "$0")" || exit 1
exec python3 server.py "$@"
