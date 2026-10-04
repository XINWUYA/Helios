#!/bin/bash
# ============================================================
#  双击入口（Finder 双击 .sh 会用文本编辑器打开，.command 才会执行）
#  逻辑全部在 Mac-GenProj-CMake.sh，这里只负责定位并转发，避免两份代码不同步。
# ============================================================

dir="$(cd -P "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
export HELIOS_DOUBLE_CLICK=1
exec "$dir/Mac-GenProj-CMake.sh" "$@"
